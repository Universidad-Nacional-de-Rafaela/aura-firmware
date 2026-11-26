/*
 * Luflex (cortina automatica) como hoja de la mesh de AURA, sobre nodo_mesh.
 *
 * Comportamiento heredado del original en MicroPython:
 *  - Calibra al arrancar midiendo el recorrido contra el Hall (cortina.c).
 *  - Modos: manual, auto1 y auto2. Arranca SIEMPRE en manual.
 *  - ABRIR/CERRAR/slider -> comando "cortina" {"posicion_pct": 0..100}; solo en modo manual.
 *  - En automatico, con la luz promedio decide 0, 50 o 100 % y lo confirma antes de mover.
 *
 * Comandos: "cortina" {"posicion_pct":N} o {"accion":"abrir"|"cerrar"}, y "led" (lo manda el boton
 * del concentrador: abre o cierra la cortina).
 * set_config: intervalo_s, modo, umbral_bajo_lux, umbral_alto_lux, confirmacion_s.
 *
 * MESH_ID_BYTES y MESH_AP_PASS: main/config_local.h (no se sube al repo).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "nodo_mesh.h"
#include "cortina.h"
#include "luz.h"
#include "logica.h"

#if __has_include("config_local.h")
#include "config_local.h"
#else
#error "Falta main/config_local.h: copia config_local.ejemplo.h como config_local.h"
#endif

#ifndef AURA_MODO_BANCO
#define AURA_MODO_BANCO 0
#endif
#if AURA_MODO_BANCO
#include "banco.h"
#endif

#define POTENCIA_TX          60          /* 15 dBm */
#define INTERVALO_DEFECTO_S  10
#define INTERVALO_MIN_S      2
#define INTERVALO_MAX_S      3600
#define LUZ_VIGENCIA_MS      5000        /* una lectura mas vieja que esto ya no vale */

/* Umbrales PROVISORIOS en lux: el original usaba valores crudos del ADC de 3 LDR (1800 y 3400)
   que no se pueden convertir. Hay que ajustarlos en el banco (ver README). */
#define UMBRAL_BAJO_DEF      200.0
#define UMBRAL_ALTO_DEF      1000.0
#define CONFIRMACION_DEF_S   5

static const char *TAG = "luflex";

static volatile uint32_t intervalo_s = INTERVALO_DEFECTO_S;
static volatile int modo = MODO_MANUAL;
static volatile double umbral_bajo = UMBRAL_BAJO_DEF;
static volatile double umbral_alto = UMBRAL_ALTO_DEF;
static volatile uint32_t confirmacion_s = CONFIRMACION_DEF_S;
static volatile bool medir_ya = false;

static volatile bool luz_ok = false;
static volatile double luz_lux = 0.0;
static volatile int64_t luz_ts_ms = 0;

static int64_t ahora_ms(void) { return (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS; }

/* ---------- Persistencia (namespace "hoja", igual que la hoja de ejemplo) ---------- */
static void nvs_guardar_u32(const char *clave, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open("hoja", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, clave, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void nvs_cargar(void)
{
    nvs_handle_t h;
    if (nvs_open("hoja", NVS_READONLY, &h) != ESP_OK) return;
    uint32_t v;
    if (nvs_get_u32(h, "intervalo_s", &v) == ESP_OK && v >= INTERVALO_MIN_S && v <= INTERVALO_MAX_S) intervalo_s = v;
    if (nvs_get_u32(h, "umbral_bajo", &v) == ESP_OK) umbral_bajo = v;
    if (nvs_get_u32(h, "umbral_alto", &v) == ESP_OK) umbral_alto = v;
    if (umbral_bajo >= umbral_alto) { umbral_bajo = UMBRAL_BAJO_DEF; umbral_alto = UMBRAL_ALTO_DEF; }
    if (nvs_get_u32(h, "confirm_s", &v) == ESP_OK && v <= 600) confirmacion_s = v;
    nvs_close(h);
}

/* ---------- Contrato AURA: configuracion ---------- */
static bool num_en_rango(const cJSON *p, double min, double max)
{
    return cJSON_IsNumber(p) && p->valuedouble >= min && p->valuedouble <= max;
}

static bool modo_desde_json(const cJSON *p, int *out)
{
    if (cJSON_IsNumber(p) && p->valuedouble >= 0 && p->valuedouble <= 2 && p->valuedouble == floor(p->valuedouble)) {
        *out = (int)p->valuedouble;
        return true;
    }
    if (cJSON_IsString(p) && p->valuestring) {
        if (strcmp(p->valuestring, "manual") == 0) { *out = MODO_MANUAL; return true; }
        if (strcmp(p->valuestring, "auto1") == 0) { *out = MODO_AUTO1; return true; }
        if (strcmp(p->valuestring, "auto2") == 0) { *out = MODO_AUTO2; return true; }
    }
    return false;
}

static bool aplicar_config(const cJSON *params, char *motivo, size_t tam)
{
    /* 1) Todo se valida primero; si algo falla no se aplica nada (contrato 3.3) */
    const cJSON *p;
    cJSON_ArrayForEach(p, params) {
        const char *k = p->string;
        if (!k) continue;
        if (strcmp(k, "aula") && strcmp(k, "intervalo_s") && strcmp(k, "modo") && strcmp(k, "umbral_bajo_lux") &&
            strcmp(k, "umbral_alto_lux") && strcmp(k, "confirmacion_s")) {
            snprintf(motivo, tam, "parametro_desconocido");
            return false;
        }
    }
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(params, "intervalo_s");
    const cJSON *mo = cJSON_GetObjectItemCaseSensitive(params, "modo");
    const cJSON *ub = cJSON_GetObjectItemCaseSensitive(params, "umbral_bajo_lux");
    const cJSON *ua = cJSON_GetObjectItemCaseSensitive(params, "umbral_alto_lux");
    const cJSON *cf = cJSON_GetObjectItemCaseSensitive(params, "confirmacion_s");
    int nuevo_modo = modo;
    if (iv && !num_en_rango(iv, INTERVALO_MIN_S, INTERVALO_MAX_S)) { snprintf(motivo, tam, "intervalo_s_fuera_de_rango"); return false; }
    if (mo && !modo_desde_json(mo, &nuevo_modo)) { snprintf(motivo, tam, "modo_invalido"); return false; }
    if (ub && !num_en_rango(ub, 0, 100000)) { snprintf(motivo, tam, "umbral_bajo_lux_fuera_de_rango"); return false; }
    if (ua && !num_en_rango(ua, 0, 100000)) { snprintf(motivo, tam, "umbral_alto_lux_fuera_de_rango"); return false; }
    if (cf && !num_en_rango(cf, 0, 600)) { snprintf(motivo, tam, "confirmacion_s_fuera_de_rango"); return false; }
    double nb = ub ? ub->valuedouble : umbral_bajo;
    double na = ua ? ua->valuedouble : umbral_alto;
    if (nb >= na) { snprintf(motivo, tam, "umbrales_incoherentes"); return false; }

    /* 2) Aplicar y guardar. El modo NO se guarda: siempre se arranca en manual, como el original. */
    if (iv) { intervalo_s = (uint32_t)iv->valuedouble; nvs_guardar_u32("intervalo_s", intervalo_s); }
    if (ub) { umbral_bajo = nb; nvs_guardar_u32("umbral_bajo", (uint32_t)nb); }
    if (ua) { umbral_alto = na; nvs_guardar_u32("umbral_alto", (uint32_t)na); }
    if (cf) { confirmacion_s = (uint32_t)cf->valuedouble; nvs_guardar_u32("confirm_s", confirmacion_s); }
    if (mo) { modo = nuevo_modo; ESP_LOGI(TAG, "Modo: %d", nuevo_modo); }
    medir_ya = true;
    return true;
}

static void describir_config(cJSON *config)
{
    cJSON_AddNumberToObject(config, "intervalo_s", intervalo_s);
    cJSON_AddNumberToObject(config, "modo", modo);
    cJSON_AddNumberToObject(config, "umbral_bajo_lux", umbral_bajo);
    cJSON_AddNumberToObject(config, "umbral_alto_lux", umbral_alto);
    cJSON_AddNumberToObject(config, "confirmacion_s", confirmacion_s);
}

/* ---------- Contrato AURA: comandos ---------- */
static bool pedir_posicion(int pct, char *motivo, size_t tam)
{
    if (modo != MODO_MANUAL) { snprintf(motivo, tam, "modo_automatico"); return false; }
    if (!cortina_mover_pct(pct)) { snprintf(motivo, tam, "cortina_no_calibrada"); return false; }
    return true;
}

static bool ejecutar_comando(const char *comando, const cJSON *params, bool local, char *motivo, size_t tam)
{
    if (strcmp(comando, "cortina") == 0) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(params, "posicion_pct");
        const cJSON *ac = cJSON_GetObjectItemCaseSensitive(params, "accion");
        if (p) {
            if (!num_en_rango(p, 0, 100)) { snprintf(motivo, tam, "posicion_fuera_de_rango"); return false; }
            return pedir_posicion((int)p->valuedouble, motivo, tam);
        }
        if (cJSON_IsString(ac) && ac->valuestring) {
            if (strcmp(ac->valuestring, "abrir") == 0) return pedir_posicion(100, motivo, tam);
            if (strcmp(ac->valuestring, "cerrar") == 0) return pedir_posicion(0, motivo, tam);
        }
        snprintf(motivo, tam, "params_invalidos");
        return false;
    }
    if (strcmp(comando, "led") == 0) {          /* boton del concentrador: abre o cierra */
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(params, "encendido");
        const cJSON *inv = cJSON_GetObjectItemCaseSensitive(params, "invertir");
        int pct;
        if (cJSON_IsBool(e)) pct = cJSON_IsTrue(e) ? 100 : 0;
        else if (cJSON_IsTrue(inv)) pct = (cortina_objetivo_pct() < 50) ? 100 : 0;
        else { snprintf(motivo, tam, "params_invalidos"); return false; }
        ESP_LOGI(TAG, "led -> cortina %d%% (orden %s)", pct, local ? "local" : "de AURA");
        return pedir_posicion(pct, motivo, tam);
    }
    snprintf(motivo, tam, "comando_desconocido");
    return false;
}

/* ---------- Tareas ---------- */
static void al_terminar_movimiento(void) { medir_ya = true; }

/* Lee la luz una vez por segundo (la usan el modo automatico y la telemetria) */
static void tarea_luz_y_auto(void *arg)
{
    logica_confirmacion_t conf = {.ultima = -1, .cuenta = 0};
    int modo_previo = modo;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        double lux;
        if (luz_leer(&lux)) {
            luz_lux = lux;
            luz_ts_ms = ahora_ms();
            luz_ok = true;
        } else if (ahora_ms() - luz_ts_ms > LUZ_VIGENCIA_MS) {
            luz_ok = false;
        }
        if (modo != modo_previo) { conf.ultima = -1; conf.cuenta = 0; modo_previo = modo; }

        if (modo == MODO_MANUAL || !luz_ok || !cortina_calibrada() || cortina_estado() != CORTINA_DETENIDA) continue;
        int sug = logica_sugerencia(modo, luz_lux, umbral_bajo, umbral_alto);
        if (logica_confirmar(&conf, sug, cortina_objetivo_pct(), (int)confirmacion_s)) {
            ESP_LOGI(TAG, "Auto%d: %.1f lx -> %d%%", modo, luz_lux, sug);
            cortina_mover_pct(sug);
        }
    }
}

static void tarea_medicion(void *arg)
{
    TickType_t ultima = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!medir_ya && ultima != 0 && (xTaskGetTickCount() - ultima) < pdMS_TO_TICKS(intervalo_s * 1000)) continue;
        medir_ya = false;
        ultima = xTaskGetTickCount();

        bool l_ok = luz_ok;
        nodo_mesh_sonda("luz_lux", l_ok, l_ok ? "" : "sin_respuesta_i2c");
        bool cal = cortina_calibrada();
        const char *err = cortina_error();
        if (cortina_estado() != CORTINA_CALIBRANDO) nodo_mesh_sonda("posicion_pct", cal, err ? err : "sin_calibrar");

        /* Solo mediciones validas: lo que falla no se manda (contrato 3.1) */
        cJSON *v = cJSON_CreateObject();
        if (l_ok) cJSON_AddNumberToObject(v, "luz_lux", luz_lux);
        if (cal) cJSON_AddNumberToObject(v, "posicion_pct", cortina_pos_pct());
        cJSON_AddNumberToObject(v, "estado_motor", cortina_estado());
        cJSON_AddNumberToObject(v, "modo", modo);
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
    nvs_cargar();

#if AURA_MODO_BANCO
    ESP_LOGW(TAG, "*** FIRMWARE DE BANCO: sin malla, consola serie ***");
    banco_correr();                      /* no vuelve */
#else
    cortina_iniciar(true, al_terminar_movimiento);
    xTaskCreate(tarea_luz_y_auto, "luz_auto", 4096, NULL, 4, NULL);

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
#endif
}
