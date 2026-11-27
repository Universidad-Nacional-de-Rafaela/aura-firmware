#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "cortina.h"

static const char *TAG = "cortina";

/* Pines por defecto (ESP32-C3 SuperMini). En banco se pueden remapear con "pines". */
static gpio_num_t pin_step = GPIO_NUM_3;
static gpio_num_t pin_dir = GPIO_NUM_4;
static gpio_num_t pin_enable = GPIO_NUM_5;   /* activo en bajo */
static gpio_num_t pin_tope = GPIO_NUM_10;    /* INPUT_PULLUP, 0 = microswitch presionado */
#define PIN_STEP    pin_step
#define PIN_DIR     pin_dir
#define PIN_ENABLE  pin_enable
#define PIN_TOPE    pin_tope
#define PIN_LED     GPIO_NUM_8               /* LED integrado C3 SuperMini (activo en bajo) */

/* Parametros de paso para microstepping 1/8 */
#define DELAY_PASO_US        500
#define DELAY_HOMING_US      1000
#define ZONA_SEGURIDAD       150             /* al cerrar, por debajo de esto el tope corta el movimiento */
#define PASOS_POR_CEDER      25              /* cada tantos pasos cede 1 tick (1 ms) */

typedef enum { ACC_HOMING, ACC_MOVER } accion_tipo_t;
typedef struct { accion_tipo_t tipo; int valor; } accion_t;

static QueueHandle_t cola = NULL;
static cortina_cb_t al_terminar_cb = NULL;
static volatile cortina_estado_t estado = CORTINA_DETENIDA;
static volatile bool calibrada = false;
static volatile bool homing_pedido = false;
static volatile bool parar = false;
static volatile int pasos_actuales = 0;
static volatile int pasos_totales = 2650;
static volatile int objetivo_pct = 0;
static const char *volatile error_cal = NULL;
static int contador_ceder = 0;

static inline void motor_on(void)  { gpio_set_level(PIN_LED, 0); gpio_set_level(PIN_ENABLE, 0); esp_rom_delay_us(10000); }
static inline void motor_off(void) { gpio_set_level(PIN_ENABLE, 1); gpio_set_level(PIN_LED, 1); }

static void dar_paso(uint32_t us)
{
    gpio_set_level(PIN_STEP, 1);
    esp_rom_delay_us(us);
    gpio_set_level(PIN_STEP, 0);
    esp_rom_delay_us(us);
    if (++contador_ceder >= PASOS_POR_CEDER) {
        contador_ceder = 0;
        vTaskDelay(1);
    }
}

static void terminar(void)
{
    motor_off();
    estado = CORTINA_DETENIDA;
    if (al_terminar_cb) al_terminar_cb();
}

static void hacer_homing(void)
{
    estado = CORTINA_CALIBRANDO;
    calibrada = false;
    error_cal = NULL;
    parar = false;
    homing_pedido = false;
    ESP_LOGI(TAG, "Homing: buscando microswitch de cierre (0 %%)...");

    /* Si ya arranca apoyada en el microswitch (ej. tras un corte estando cerrada), ya esta en 0 % */
    if (gpio_get_level(PIN_TOPE) == 0) {
        pasos_actuales = 0;
        objetivo_pct = 0;
        calibrada = true;
        ESP_LOGI(TAG, "Homing OK: ya estaba en el tope (0 %%)");
        terminar();
        return;
    }

    motor_on();
    gpio_set_level(PIN_DIR, 0);
    int limite = pasos_totales + (pasos_totales / 2) + 500;
    int contador = 0;
    while (gpio_get_level(PIN_TOPE) == 1) {
        dar_paso(DELAY_HOMING_US);
        contador++;
        if (parar) { error_cal = "homing_cancelado"; terminar(); return; }
        if (contador >= limite) {
            error_cal = "homing_sin_tope";
            ESP_LOGE(TAG, "Homing abortado: %d pasos sin activar el microswitch", contador);
            terminar();
            return;
        }
    }

    /* Se detiene en cuanto activa el microswitch, sin empujar contra el tope */
    pasos_actuales = 0;
    objetivo_pct = 0;
    calibrada = true;
    ESP_LOGI(TAG, "Homing OK tras %d pasos (recorrido configurado: %d pasos)", contador, pasos_totales);
    terminar();
}

static void hacer_movimiento(int pct)
{
    if (!calibrada || pasos_totales <= 0) {
        terminar();
        return;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int destino = (int)(((int64_t)pct * pasos_totales) / 100);
    if (destino == pasos_actuales) {
        objetivo_pct = pct;
        terminar();
        return;
    }

    parar = false;
    estado = CORTINA_MOVIENDO;
    bool subiendo = destino > pasos_actuales;
    ESP_LOGI(TAG, "Yendo al %d%% (pasos %d -> %d)", pct, pasos_actuales, destino);
    motor_on();
    gpio_set_level(PIN_DIR, subiendo ? 1 : 0);

    int n = 0;
    while (pasos_actuales != destino) {
        dar_paso(DELAY_PASO_US);
        pasos_actuales += subiendo ? 1 : -1;
        /* Corte de seguridad al cerrar: el microswitch detiene el motor en 0 % */
        if (!subiendo && pasos_actuales < ZONA_SEGURIDAD && gpio_get_level(PIN_TOPE) == 0) {
            pasos_actuales = 0;
            break;
        }
        if (parar) break;
        if (++n % PASOS_POR_CEDER == 0 && uxQueueMessagesWaiting(cola) > 0) {
            ESP_LOGI(TAG, "Comando nuevo: se reemplaza el movimiento en curso");
            return;                      /* la tarea toma enseguida la accion pendiente */
        }
    }
    objetivo_pct = parar ? cortina_pos_pct() : pct;
    ESP_LOGI(TAG, "Movimiento finalizado: %d%%", cortina_pos_pct());
    terminar();
}

static void tarea_motor(void *arg)
{
    accion_t a;
    for (;;) {
        if (xQueueReceive(cola, &a, portMAX_DELAY) != pdTRUE) continue;
        if (a.tipo == ACC_HOMING) hacer_homing();
        else hacer_movimiento(a.valor);
    }
}

void cortina_iniciar(int pasos_totales_inicial, bool homing_al_arrancar, cortina_cb_t cb)
{
    if (pasos_totales_inicial > 0) pasos_totales = pasos_totales_inicial;
    al_terminar_cb = cb;
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_STEP) | (1ULL << PIN_DIR) | (1ULL << PIN_ENABLE) | (1ULL << PIN_LED),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(PIN_STEP, 0);
    motor_off();
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_TOPE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);

    cola = xQueueCreate(1, sizeof(accion_t));
    if (homing_al_arrancar) {
        estado = CORTINA_CALIBRANDO;
        homing_pedido = true;
        accion_t a = {ACC_HOMING, 0};
        xQueueOverwrite(cola, &a);
    }
    xTaskCreate(tarea_motor, "motor", 4096, NULL, 3, NULL);
}

void cortina_fijar_pasos_totales(int pasos)
{
    if (pasos <= 0) return;
    int pct_actual = cortina_pos_pct();
    pasos_totales = pasos;
    if (calibrada && pct_actual >= 0) {
        pasos_actuales = (int)(((int64_t)pct_actual * pasos_totales) / 100);
    }
}

void cortina_homing(void)
{
    homing_pedido = true;
    parar = true;
    accion_t a = {ACC_HOMING, 0};
    xQueueOverwrite(cola, &a);
}

bool cortina_mover_pct(int pct)
{
    if (!calibrada || homing_pedido || estado == CORTINA_CALIBRANDO) return false;
    parar = false;
    objetivo_pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    accion_t a = {ACC_MOVER, pct};
    xQueueOverwrite(cola, &a);
    return true;
}

void cortina_parar(void) { parar = true; }

cortina_estado_t cortina_estado(void) { return estado; }
bool cortina_calibrada(void) { return calibrada; }
int  cortina_pos_pct(void) { return calibrada && pasos_totales > 0 ? (int)(((int64_t)pasos_actuales * 100) / pasos_totales) : -1; }
int  cortina_objetivo_pct(void) { return objetivo_pct; }
int  cortina_pasos_actuales(void) { return pasos_actuales; }
int  cortina_pasos_totales(void) { return pasos_totales; }
const char *cortina_error(void) { return error_cal; }

/* ---- Banco ---- */
void cortina_banco_habilitar(bool en) { if (en) motor_on(); else motor_off(); }
void cortina_banco_direccion(int nivel) { gpio_set_level(PIN_DIR, nivel ? 1 : 0); }
void cortina_banco_pasos(int n, uint32_t us)
{
    for (int i = 0; i < n; i++) dar_paso(us);
}
int cortina_tope(void) { return gpio_get_level(PIN_TOPE); }

void cortina_banco_pines(int step, int dir, int ena, int tope)
{
    motor_off();
    gpio_reset_pin(pin_step); gpio_reset_pin(pin_dir); gpio_reset_pin(pin_enable); gpio_reset_pin(pin_tope);
    pin_step = (gpio_num_t)step; pin_dir = (gpio_num_t)dir; pin_enable = (gpio_num_t)ena; pin_tope = (gpio_num_t)tope;
    gpio_set_level(pin_enable, 1);
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << pin_step) | (1ULL << pin_dir) | (1ULL << pin_enable),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(pin_step, 0);
    gpio_set_level(pin_enable, 1);
    gpio_config_t in = {.pin_bit_mask = 1ULL << pin_tope, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&in);
}

void cortina_banco_pines_actuales(int *step, int *dir, int *ena, int *tope)
{
    *step = pin_step; *dir = pin_dir; *ena = pin_enable; *tope = pin_tope;
}
