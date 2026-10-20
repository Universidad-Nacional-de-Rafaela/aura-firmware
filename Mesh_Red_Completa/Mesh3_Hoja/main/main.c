/*
 * Hoja de ejemplo de la mesh de AURA (ESP32-C3 SuperMini) sobre la biblioteca nodo_mesh.
 * Es la BASE para los dispositivos de los grupos: lo propio del dispositivo es solo esto
 * (qué mide, qué parámetros tiene y qué comandos acepta). La conexión, el aula, la cola,
 * la confirmación de AURA y la hora las resuelve nodo_mesh (componentes/nodo_mesh).
 *
 * Este ejemplo:
 *  - Mide cada "intervalo_s" (datos simulados: temp_c, hum_pct, presencia) y el estado del LED.
 *  - Parámetro "intervalo_s" (2 a 3600 s), que se cambia con set_config y se guarda en NVS.
 *  - Comando "led": {"command":"led","params":{"encendido":true}} o {"invertir":true}.
 *    También llega del botón del concentrador (comando local, sin respuesta a AURA).
 *
 * Todas las hojas iguales llevan este MISMO binario: no hay MAC, id ni aula en el código.
 * El aula la asigna AURA con set_config {"aula":"aula1"}.
 *
 * MESH_ID y MESH_AP_PASS: en config_local.h (igual que en el gateway y el concentrador).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "nodo_mesh.h"

#if __has_include("config_local.h")
#include "config_local.h"
#else
#error "Falta main/config_local.h: copiá config_local.ejemplo.h como config_local.h y completá el ID y la clave de la mesh"
#endif

#define PIN_LED             GPIO_NUM_8         /* LED azul de la C3 SuperMini */
#define LED_ACTIVO_BAJO     1
/* Potencia en pasos de 0,25 dBm. La C3 SuperMini tiene la antena mal adaptada: con la de
   fábrica no se conectaba. 60 = 15 dBm, con antena de alambre de 31 mm (~10 m por una vidriera). */
#define POTENCIA_TX         60
#define INTERVALO_DEFECTO_S 10
#define INTERVALO_MIN_S     2
#define INTERVALO_MAX_S     3600

static const char *TAG = "HOJA";
static volatile uint32_t intervalo_s = INTERVALO_DEFECTO_S;
static volatile bool led = false;
static volatile bool medir_ya = false;

/* ---------- Lo que aporta el dispositivo ---------- */
static void led_aplicar(bool encendido)
{
    led = encendido;
    gpio_set_level(PIN_LED, LED_ACTIVO_BAJO ? !encendido : encendido);
}

static bool aplicar_config(const cJSON *params, char *motivo, size_t tam)
{
    /* Primero se valida TODO; solo después se aplica (contrato §3.3) */
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(params, "intervalo_s");
    if (iv != NULL && (!cJSON_IsNumber(iv) || iv->valuedouble < INTERVALO_MIN_S || iv->valuedouble > INTERVALO_MAX_S)) {
        snprintf(motivo, tam, "intervalo_s_fuera_de_rango");
        return false;
    }
    const cJSON *p;
    cJSON_ArrayForEach(p, params) {
        if (strcmp(p->string, "intervalo_s") != 0 && strcmp(p->string, "aula") != 0) {
            snprintf(motivo, tam, "parametro_desconocido");
            return false;
        }
    }
    if (iv != NULL) {
        intervalo_s = (uint32_t)iv->valuedouble;
        nvs_handle_t h;
        if (nvs_open("hoja", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u32(h, "intervalo_s", intervalo_s);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "Nuevo intervalo: %lu s", (unsigned long)intervalo_s);
    }
    return true;
}

static void describir_config(cJSON *config)
{
    cJSON_AddNumberToObject(config, "intervalo_s", intervalo_s);
}

static bool ejecutar_comando(const char *comando, const cJSON *params, bool local, char *motivo, size_t tam)
{
    if (strcmp(comando, "led") != 0) {
        snprintf(motivo, tam, "comando_desconocido");
        return false;
    }
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(params, "encendido");
    const cJSON *inv = cJSON_GetObjectItemCaseSensitive(params, "invertir");
    if (cJSON_IsBool(e)) {
        led_aplicar(cJSON_IsTrue(e));
    } else if (cJSON_IsTrue(inv)) {
        led_aplicar(!led);
    } else {
        snprintf(motivo, tam, "params_invalidos");
        return false;
    }
    ESP_LOGI(TAG, "LED %s (orden %s)", led ? "ENCENDIDO" : "apagado", local ? "local" : "de AURA");
    medir_ya = true;                   /* que AURA y el concentrador vean el estado nuevo ya */
    return true;
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
        /* Datos simulados, en double y con un decimal (cJSON escribe 24.1 y no 24.100000381) */
        cJSON *v = cJSON_CreateObject();
        cJSON_AddNumberToObject(v, "temp_c", 20.0 + (double)(esp_random() % 100) / 10.0);
        cJSON_AddNumberToObject(v, "hum_pct", 40.0 + (double)(esp_random() % 300) / 10.0);
        cJSON_AddBoolToObject(v, "presencia", esp_random() % 2);
        cJSON_AddNumberToObject(v, "led", led ? 1 : 0);
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

    nodo_mesh_config_t cfg = {
        .mesh_id = {MESH_ID_BYTES},
        .mesh_clave = MESH_AP_PASS,
        .potencia_tx = POTENCIA_TX,
        .alimentacion = "red",
        .aplicar_config = aplicar_config,
        .describir_config = describir_config,
        .ejecutar_comando = ejecutar_comando,
    };
    ESP_ERROR_CHECK(nodo_mesh_iniciar(&cfg));
    xTaskCreate(tarea_medicion, "medicion", 4096, NULL, 4, NULL);
}
