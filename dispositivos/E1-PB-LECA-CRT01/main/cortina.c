#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "cortina.h"

static const char *TAG = "cortina";

/* ---- Pines (ESP32-C3 SuperMini) ---- */
/* Pines por defecto. En el banco se pueden remapear con cortina_banco_pines() (comando "pines"). */
static gpio_num_t pin_step = GPIO_NUM_3;
static gpio_num_t pin_dir = GPIO_NUM_4;
static gpio_num_t pin_enable = GPIO_NUM_5;   /* activo en bajo (igual que el original) */
static gpio_num_t pin_hall = GPIO_NUM_10;    /* INPUT_PULLUP, 0 = tope/iman activo */
#define PIN_STEP    pin_step
#define PIN_DIR     pin_dir
#define PIN_ENABLE  pin_enable
#define PIN_HALL    pin_hall
#define PIN_LED     GPIO_NUM_8      /* LED integrado C3 SuperMini (activo en bajo) */

/* ---- Parametros heredados del original ---- */
#define DELAY_PASO_US        500    /* semiperiodo de paso en movimiento normal (1/8 micropaso) */
#define DELAY_CAL_US         1000   /* semiperiodo en calibracion */
#define CAL_MAX_PASOS        30000
#define CAL_MIN_PASOS        200    /* menos que esto: el Hall estaba casi encima, la medida no sirve */
#define SOBRERRECORRIDO      40     /* pasos que sigue despues de ver el Hall */
#define ZONA_SEGURIDAD       150    /* al cerrar, por debajo de esto el Hall corta el movimiento */
#define PASOS_POR_CEDER      25     /* cada tantos pasos se cede la CPU 1 tick (tick = 1 ms) */

typedef enum { ACC_CALIBRAR, ACC_MOVER } accion_tipo_t;
typedef struct { accion_tipo_t tipo; int valor; } accion_t;

static QueueHandle_t cola = NULL;
static cortina_cb_t al_terminar_cb = NULL;
static volatile cortina_estado_t estado = CORTINA_DETENIDA;
static volatile bool calibrada = false;
static volatile bool calibracion_pedida = false;
static volatile bool parar = false;
static volatile int pasos_actuales = 0;
static volatile int pasos_totales = 0;
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
    if (++contador_ceder >= PASOS_POR_CEDER) {   /* sin esto salta el watchdog y se frena la malla */
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

static void hacer_calibracion(void)
{
    estado = CORTINA_CALIBRANDO;
    calibrada = false;
    error_cal = NULL;
    parar = false;
    calibracion_pedida = false;
    ESP_LOGI(TAG, "Calibrando: buscando el Hall...");

    if (gpio_get_level(PIN_HALL) == 0) {
        error_cal = "hall_activo_al_inicio";
        ESP_LOGE(TAG, "El Hall ya esta activo al iniciar: no se puede medir el recorrido");
        terminar();
        return;
    }
    motor_on();
    gpio_set_level(PIN_DIR, 0);
    int contador = 0;
    while (gpio_get_level(PIN_HALL) == 1) {
        dar_paso(DELAY_CAL_US);
        contador++;
        if (parar) { error_cal = "calibracion_cancelada"; terminar(); return; }
        if (contador >= CAL_MAX_PASOS) {
            error_cal = "calibracion_sin_hall";
            ESP_LOGE(TAG, "Calibracion abortada: %d pasos sin ver el Hall", contador);
            terminar();
            return;
        }
    }
    for (int i = 0; i < SOBRERRECORRIDO; i++) dar_paso(DELAY_CAL_US);

    if (contador < CAL_MIN_PASOS) {
        error_cal = "recorrido_muy_corto";
        ESP_LOGE(TAG, "Recorrido medido sospechosamente corto: %d pasos", contador);
        terminar();
        return;
    }
    pasos_totales = contador;
    pasos_actuales = 0;
    objetivo_pct = 0;
    calibrada = true;
    ESP_LOGI(TAG, "Calibracion OK: recorrido de %d pasos", contador);
    terminar();
}

static void hacer_movimiento(int pct)
{
    if (!calibrada || pasos_totales == 0) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int destino = (int)(((int64_t)pct * pasos_totales) / 100);
    if (destino == pasos_actuales) { objetivo_pct = pct; return; }

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
        /* Corte de seguridad: al cerrar, cerca del extremo, el Hall manda (el original lo hacia en
           ambos sentidos, lo que podia trabar la apertura si el iman era ancho). */
        if (!subiendo && pasos_actuales < ZONA_SEGURIDAD && gpio_get_level(PIN_HALL) == 0) {
            for (int i = 0; i < SOBRERRECORRIDO; i++) dar_paso(DELAY_PASO_US);
            pasos_actuales = 0;
            break;
        }
        if (parar) break;
        if (++n % PASOS_POR_CEDER == 0 && uxQueueMessagesWaiting(cola) > 0) {
            ESP_LOGI(TAG, "Comando nuevo: se reemplaza el movimiento en curso");
            return;                      /* la tarea toma enseguida la accion pendiente */
        }
    }
    objetivo_pct = pct;
    if (parar) objetivo_pct = (int)(((int64_t)pasos_actuales * 100) / pasos_totales);
    ESP_LOGI(TAG, "Movimiento finalizado: %d%%", cortina_pos_pct());
    terminar();
}

static void tarea_motor(void *arg)
{
    accion_t a;
    for (;;) {
        if (xQueueReceive(cola, &a, portMAX_DELAY) != pdTRUE) continue;
        if (a.tipo == ACC_CALIBRAR) hacer_calibracion();
        else hacer_movimiento(a.valor);
    }
}

void cortina_iniciar(bool calibrar_al_arrancar, cortina_cb_t cb)
{
    al_terminar_cb = cb;
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_STEP) | (1ULL << PIN_DIR) | (1ULL << PIN_ENABLE) | (1ULL << PIN_LED),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(PIN_STEP, 0);
    motor_off();
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIN_HALL,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);

    cola = xQueueCreate(1, sizeof(accion_t));
    if (calibrar_al_arrancar) {
        estado = CORTINA_CALIBRANDO;
        calibracion_pedida = true;
        accion_t a = {ACC_CALIBRAR, 0};
        xQueueOverwrite(cola, &a);
    }
    /* Prioridad baja: la malla y WiFi tienen que poder interrumpirla */
    xTaskCreate(tarea_motor, "motor", 4096, NULL, 3, NULL);
}

void cortina_calibrar(void)
{
    calibracion_pedida = true;
    parar = true;                        /* corta lo que este haciendo */
    accion_t a = {ACC_CALIBRAR, 0};
    xQueueOverwrite(cola, &a);
}

bool cortina_mover_pct(int pct)
{
    if (!calibrada || calibracion_pedida || estado == CORTINA_CALIBRANDO) return false;
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
void cortina_banco_fijar_totales(int pasos)
{
    pasos_totales = pasos;
    pasos_actuales = 0;
    objetivo_pct = 0;
    calibrada = pasos > 0;
    error_cal = NULL;
}
int cortina_hall(void) { return gpio_get_level(PIN_HALL); }

void cortina_banco_pines(int step, int dir, int ena, int hall)
{
    motor_off();
    gpio_reset_pin(pin_step); gpio_reset_pin(pin_dir); gpio_reset_pin(pin_enable); gpio_reset_pin(pin_hall);
    pin_step = (gpio_num_t)step; pin_dir = (gpio_num_t)dir; pin_enable = (gpio_num_t)ena; pin_hall = (gpio_num_t)hall;
    gpio_set_level(pin_enable, 1);                 /* driver deshabilitado antes de ser salida */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << pin_step) | (1ULL << pin_dir) | (1ULL << pin_enable),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(pin_step, 0);
    gpio_set_level(pin_enable, 1);
    gpio_config_t in = {.pin_bit_mask = 1ULL << pin_hall, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&in);
}

void cortina_banco_pines_actuales(int *step, int *dir, int *ena, int *hall)
{
    *step = pin_step; *dir = pin_dir; *ena = pin_enable; *hall = pin_hall;
}
