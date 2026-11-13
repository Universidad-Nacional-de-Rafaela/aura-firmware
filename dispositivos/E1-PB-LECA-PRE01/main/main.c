/*
 * E1-PB-LECA-PRE01 — Hoja de presencia por cámara (proyecto MCCV, IC IV 2026).
 *
 * La detección la hace una Raspberry Pi con cámara. La Pi le manda a esta hoja, por UART,
 * una línea de texto con el estado (ver "Protocolo con la Pi" abajo). La hoja publica la
 * medición por la mesh; la conexión, el aula, la cola y la confirmación de AURA las resuelve
 * la biblioteca nodo_mesh.
 *
 *   values: {"presencia": true, "conf_pct": 72}
 *
 * Sin dato falso (contrato §3.1): si la Pi avisa que la cámara no da una lectura confiable
 * ("U"), o pasa PI_TIMEOUT_S sin recibir una línea válida, NO se publica medición; la falla se
 * informa con la alerta "sensor" (motivo sin_respuesta) y, cuando vuelve, "recuperada".
 *
 * Protocolo con la Pi (115200 8N1, una línea ASCII terminada en \n, la Pi la repite cada 5 s):
 *   "P1 72"  hay personas, confianza 72 %      "P0 90"  no hay personas, confianza 90 %
 *   "P1"     hay personas, sin confianza       "P0"     no hay personas, sin confianza
 *   "U"      la cámara no da una lectura confiable
 * Cualquier otra cosa se descarta y se loguea.
 *
 * Conexión:  Pi GPIO14 TXD (pin 8)  -> C3 GPIO4 (RX)
 *            Pi GPIO15 RXD (pin 10) <- C3 GPIO5 (TX)   (no se usa hoy, queda cableado)
 *            Pi GND (pin 6)         -- C3 GND
 *
 * Configuración remota (set_config): intervalo_s (2 a 3600 s), validado y guardado en NVS.
 * MESH_ID y MESH_AP_PASS: en config_local.h (lo da la cátedra, no se sube).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "cJSON.h"
#include "nodo_mesh.h"
#include "protocolo_pi.h"

#if __has_include("config_local.h")
#include "config_local.h"
#else
#error "Falta main/config_local.h: copiá config_local.ejemplo.h como config_local.h y completá el ID y la clave de la mesh"
#endif

#define PIN_LED             GPIO_NUM_8         /* LED azul de la C3 SuperMini: encendido = presencia */
#define LED_ACTIVO_BAJO     1
#define POTENCIA_TX         60                 /* 15 dBm, con antena de alambre de 31 mm */

#define PI_UART             UART_NUM_1
#define PI_PIN_RX           GPIO_NUM_4
#define PI_PIN_TX           GPIO_NUM_5
#define PI_BAUDIOS          115200
#define LINEA_MAX           32
#define PI_TIMEOUT_S        30                 /* sin líneas válidas en este tiempo = sensor sin respuesta */

#define INTERVALO_DEFECTO_S 10
#define INTERVALO_MIN_S     2
#define INTERVALO_MAX_S     3600

static const char *TAG = "PRESENCIA";
static volatile uint32_t intervalo_s = INTERVALO_DEFECTO_S;
static volatile bool medir_ya = false;

/* Último estado recibido de la Pi. Se protege con un mutex porque lo escribe la tarea del
   UART y lo lee la de medición. */
typedef struct {
    linea_pi_t linea;          /* lo último que dijo la Pi */
    TickType_t recibido;       /* tick de la última línea válida; 0 = nunca */
} estado_pi_t;

static estado_pi_t estado = {.linea = {.confiable = false, .presencia = false, .conf_pct = -1}, .recibido = 0};
static SemaphoreHandle_t mutex_estado;

/* ---------- Protocolo con la Pi ---------- */

static void procesar_linea(const char *linea)
{
    linea_pi_t nueva;
    if (!protocolo_pi_parsear(linea, &nueva)) {
        ESP_LOGW(TAG, "Línea de la Pi descartada: \"%s\"", linea);
        return;
    }
    xSemaphoreTake(mutex_estado, portMAX_DELAY);
    bool cambio = (nueva.confiable != estado.linea.confiable) || (nueva.presencia != estado.linea.presencia)
                  || estado.recibido == 0;
    TickType_t ahora = xTaskGetTickCount();
    estado.linea = nueva;
    estado.recibido = (ahora == 0) ? 1 : ahora;    /* 0 queda reservado para "nunca" */
    xSemaphoreGive(mutex_estado);
    if (cambio) {
        ESP_LOGI(TAG, "Pi: %s", linea);
        medir_ya = true;       /* que AURA vea el cambio ya, sin esperar el intervalo */
    }
}

static void tarea_uart(void *arg)
{
    uint8_t byte;
    char linea[LINEA_MAX];
    size_t largo = 0;
    bool descartando = false;    /* línea demasiado larga: se ignora hasta el próximo \n */

    while (true) {
        int n = uart_read_bytes(PI_UART, &byte, 1, pdMS_TO_TICKS(1000));
        if (n <= 0) {
            continue;
        }
        if (byte == '\r') {
            continue;
        }
        if (byte == '\n') {
            if (!descartando && largo > 0) {
                linea[largo] = '\0';
                procesar_linea(linea);
            }
            largo = 0;
            descartando = false;
            continue;
        }
        if (largo >= LINEA_MAX - 1) {
            descartando = true;
            continue;
        }
        linea[largo++] = (char)byte;
    }
}

/* ---------- Lo que aporta el dispositivo a nodo_mesh ---------- */

/* true si item es un número entero entre min y max (2.5 o "10" no valen). */
static bool entero_en_rango(const cJSON *item, double min, double max)
{
    return cJSON_IsNumber(item) && item->valuedouble == (double)(int64_t)item->valuedouble
           && item->valuedouble >= min && item->valuedouble <= max;
}

/* Valida TODO antes de aplicar algo (contrato §3.3). "aula" la maneja la biblioteca. */
static bool aplicar_config(const cJSON *params, char *motivo, size_t tam)
{
    const cJSON *p;
    cJSON_ArrayForEach(p, params) {
        if (strcmp(p->string, "intervalo_s") != 0 && strcmp(p->string, "aula") != 0) {
            snprintf(motivo, tam, "parametro_desconocido");
            return false;
        }
    }
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(params, "intervalo_s");
    if (iv == NULL) {
        return true;               /* solo aula: la maneja la biblioteca */
    }
    if (!entero_en_rango(iv, INTERVALO_MIN_S, INTERVALO_MAX_S)) {
        snprintf(motivo, tam, "intervalo_s_fuera_de_rango");
        return false;
    }

    /* Primero se guarda y, solo si se pudo, se aplica: lo que responde la placa es lo que va a
       tener después de un reinicio. */
    nvs_handle_t h;
    if (nvs_open("hoja", NVS_READWRITE, &h) != ESP_OK) {
        snprintf(motivo, tam, "error_de_nvs");
        return false;
    }
    esp_err_t err = nvs_set_u32(h, "intervalo_s", (uint32_t)iv->valuedouble);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        snprintf(motivo, tam, "error_de_nvs");
        return false;
    }
    intervalo_s = (uint32_t)iv->valuedouble;
    ESP_LOGI(TAG, "Nuevo intervalo: %lu s", (unsigned long)intervalo_s);
    return true;
}

static void describir_config(cJSON *config)
{
    cJSON_AddNumberToObject(config, "intervalo_s", intervalo_s);
}

static void led_aplicar(bool encendido)
{
    gpio_set_level(PIN_LED, LED_ACTIVO_BAJO ? !encendido : encendido);
}

/* ---------- Medición ---------- */
static void tarea_medicion(void *arg)
{
    TickType_t ultima = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!medir_ya && ultima != 0 && (xTaskGetTickCount() - ultima) < pdMS_TO_TICKS(intervalo_s * 1000)) {
            continue;
        }
        medir_ya = false;
        ultima = xTaskGetTickCount();

        xSemaphoreTake(mutex_estado, portMAX_DELAY);
        estado_pi_t e = estado;
        xSemaphoreGive(mutex_estado);

        bool vigente = e.recibido != 0 && (ultima - e.recibido) < pdMS_TO_TICKS(PI_TIMEOUT_S * 1000);
        bool ok = vigente && e.linea.confiable;

        /* La alerta "sensor" sale solo cuando cambia (la biblioteca lo controla). */
        nodo_mesh_sonda("presencia", ok, "sin_respuesta");
        led_aplicar(ok && e.linea.presencia);
        if (!ok) {
            continue;              /* sin dato falso: no se publica medición */
        }

        cJSON *v = cJSON_CreateObject();
        cJSON_AddBoolToObject(v, "presencia", e.linea.presencia);
        if (e.linea.conf_pct >= 0) {
            cJSON_AddNumberToObject(v, "conf_pct", e.linea.conf_pct);
        }
        nodo_mesh_medicion(v);
        cJSON_Delete(v);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_reset_pin(PIN_LED);
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    led_aplicar(false);

    nvs_handle_t h;
    if (nvs_open("hoja", NVS_READONLY, &h) == ESP_OK) {
        uint32_t v;
        if (nvs_get_u32(h, "intervalo_s", &v) == ESP_OK && v >= INTERVALO_MIN_S && v <= INTERVALO_MAX_S) {
            intervalo_s = v;
        }
        nvs_close(h);
    }

    mutex_estado = xSemaphoreCreateMutex();
    const uart_config_t uart_cfg = {
        .baud_rate = PI_BAUDIOS,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(PI_UART, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PI_UART, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(PI_UART, PI_PIN_TX, PI_PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    nodo_mesh_config_t cfg = {
        .mesh_id = {MESH_ID_BYTES},
        .mesh_clave = MESH_AP_PASS,
        .potencia_tx = POTENCIA_TX,
        .alimentacion = "red",
        .aplicar_config = aplicar_config,
        .describir_config = describir_config,
        .ejecutar_comando = NULL,               /* solo mide: sin comandos propios */
    };
    ESP_ERROR_CHECK(nodo_mesh_iniciar(&cfg));
    xTaskCreate(tarea_uart, "uart_pi", 3072, NULL, 5, NULL);
    xTaskCreate(tarea_medicion, "medicion", 4096, NULL, 4, NULL);
}
