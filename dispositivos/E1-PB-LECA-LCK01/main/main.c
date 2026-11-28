/*
 * ============================================================================================
 *  E1-PB-LECA-LCK01 — hoja de la mesh de AURA para el gabinete de lockers del pañol (LabECA)
 *  Tipo de firmware aura-lockers (ESP-IDF 6.1, ESP32-C3 SuperMini). Una placa, 4 lockers.
 * ============================================================================================
 *
 *  Cada locker es una puerta con cerradura por pulso y sensor de apertura (ADR-009). Sobre la
 *  biblioteca nodo_mesh, que resuelve la malla, el aula, la cola con confirmación, la hora, la
 *  OTA y la firma. Lo propio del gabinete (contrato v4.7, propuesto en este mismo PR):
 *
 *   QUÉ MIDE   en cada cambio, y una completa cada intervalo_s:
 *                {"puerta_1":0,…,"puerta_4":0,"cerradura_1":0,…,"cerradura_4":0}
 *              puerta_N    1 = abierto (microswitch libre o cable cortado), 0 = cerrado
 *              cerradura_N 1 = energizada (se puede abrir), 0 = trabada
 *
 *   COMANDOS   abrir   {"puerta": <1 a 4>, "vence": <segundos Unix>}. DESHABILITADO AL COMPILAR
 *                      (ADR-009, decisión 8): con CONFIG_LOCKER_ABRIR_REMOTO apagada (por defecto)
 *                      se rechaza con abrir_deshabilitado. Encendida (solo en el banco): rechaza
 *                      puerta_invalida, sin_vencimiento, sin_hora_valida, orden_vencida,
 *                      vencimiento_muy_lejano y, con CUALQUIER cerradura energizada o el límite
 *                      por minuto de esa cerradura alcanzado, ocupada. Un pulso a la vez en todo
 *                      el gabinete: la fuente es una, de 5 A, y cada cerradura consume ~2,2 A.
 *              estado  {} de solo lectura: manda el reporte y una medición en el momento.
 *              Una orden del botón del concentrador (local) solo puede ser "estado".
 *
 *   SET_CONFIG intervalo_s (5 a 3600; 30) · pulso_ms (500 a 5000; 3000) · aviso_s (0 = sin aviso,
 *              o 5 a 600; 120). Todo o nada, enteros, guardado en NVS. Valen para los 4 lockers.
 *
 *   ALERTAS    (contrato v4.5, §3.5, con "puerta": <n> en details) intrusion (critical) ·
 *              arranque_puerta_abierta (high) · puerta_abierta (warning) · puerta_cerrada (info) ·
 *              limite_pulsos (warning).
 *
 *   REPORTE    config.abiertas = [0,1,0,0], de solo lectura.
 *
 *  Las cerraduras son DFRobot FIT0620: el fabricante pide no energizarlas más de 5 s seguidos.
 *  Tope de 5 s por pulso y 20 s energizada en cualquier minuto, por cerradura
 *  (cerradura_pulso.h); el corte lo hace un esp_timer por locker (salida_cerradura.c) y la tarea
 *  está suscrita al watchdog.
 *
 *  Todas las placas de gabinete llevan ESTE MISMO binario: no hay MAC, código ni aula en el
 *  código. MESH_ID_BYTES y MESH_AP_PASS van en main/config_local.h (no se sube).
 * ============================================================================================
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "nodo_mesh.h"

#include "pines.h"
#include "salida_cerradura.h"
#include "locker_logica.h"
#include "config_validacion.h"

#if __has_include("config_local.h")
#include "config_local.h"
#else
#error "Falta main/config_local.h: copiá config_local.ejemplo.h como config_local.h y completá el ID y la clave de la mesh"
#endif

#if CONFIG_LOCKER_CORTAR_AL_ABRIR
#define CORTAR_AL_ABRIR true
#else
#define CORTAR_AL_ABRIR false
#endif
#if CONFIG_LOCKER_ABRIR_REMOTO
#define ABRIR_REMOTO true
#else
#define ABRIR_REMOTO false
#endif

/* Potencia en pasos de 0,25 dBm. La C3 SuperMini tiene la antena mal adaptada: la cátedra
   usa 60 (15 dBm) con un alambre de 31 mm. No subir: la misma fuente alimenta la cerradura. */
#define POTENCIA_TX         60

#define INTERVALO_DEFECTO_S 30
#define INTERVALO_MIN_S     5
#define INTERVALO_MAX_S     3600

#define PERIODO_LOGICA_MS   10
#define NVS_ESPACIO         "locker"
#define NVS_CLAVE_CONFIG    "config"

static const char *TAG = "GABINETE";

typedef struct {
    int32_t intervalo_s;
    int32_t pulso_ms;
    int32_t aviso_s;
} config_locker_t;

static config_locker_t cfg = {INTERVALO_DEFECTO_S, PULSO_DEFECTO_MS, AVISO_DEFECTO_S};
static SemaphoreHandle_t lock_cfg;           /* cfg: la cambia set_config (tarea de nodo_mesh) */
static SemaphoreHandle_t lock_locker;        /* gabinete: lo usan su tarea, "abrir" y el reporte */
static gabinete_t gabinete;
static volatile bool medir_ya = false;

static inline int64_t ahora_ms(void) { return esp_timer_get_time() / 1000; }

static config_locker_t cfg_leer(void)
{
    xSemaphoreTake(lock_cfg, portMAX_DELAY);
    config_locker_t k = cfg;
    xSemaphoreGive(lock_cfg);
    return k;
}

/* ============================== NVS ============================== */
static bool config_valida(const config_locker_t *k)
{
    return k->intervalo_s >= INTERVALO_MIN_S && k->intervalo_s <= INTERVALO_MAX_S &&
           k->pulso_ms >= PULSO_MINIMO_MS && k->pulso_ms <= PULSO_TOPE_MS && aviso_valido(k->aviso_s);
}

static void config_cargar(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_ESPACIO, NVS_READONLY, &h) != ESP_OK) {
        return;                                          /* primera vez: valores por defecto */
    }
    config_locker_t k;
    size_t tam = sizeof(k);
    if (nvs_get_blob(h, NVS_CLAVE_CONFIG, &k, &tam) == ESP_OK && tam == sizeof(k) && config_valida(&k)) {
        cfg = k;
    }
    nvs_close(h);
}

/* Un solo blob: la configuración se guarda toda o nada */
static bool config_guardar(const config_locker_t *k)
{
    nvs_handle_t h;
    if (nvs_open(NVS_ESPACIO, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, NVS_CLAVE_CONFIG, k, sizeof(*k)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* ============================== set_config ============================== */
/* Primero se valida TODO y recién después se aplica (contrato §3.3). "aula" la maneja nodo_mesh. */
static bool leer_param(const cJSON *p, int32_t min, int32_t max, int32_t *valor)
{
    return cJSON_IsNumber(p) && entero_en_rango(p->valuedouble, min, max, valor);
}

static bool aplicar_config(const cJSON *params, char *motivo, size_t tam)
{
    if (!cJSON_IsObject(params)) {
        snprintf(motivo, tam, "params_invalido");
        return false;
    }
    config_locker_t nueva = cfg_leer();
    const cJSON *p;
    cJSON_ArrayForEach(p, params) {
        int32_t v;
        if (p->string == NULL) {
            snprintf(motivo, tam, "params_invalido");
            return false;
        }
        if (strcmp(p->string, "aula") == 0) continue;
        if (strcmp(p->string, "intervalo_s") == 0) {
            if (!leer_param(p, INTERVALO_MIN_S, INTERVALO_MAX_S, &v)) {
                snprintf(motivo, tam, "intervalo_s_fuera_de_rango");
                return false;
            }
            nueva.intervalo_s = v;
        } else if (strcmp(p->string, "pulso_ms") == 0) {
            if (!leer_param(p, PULSO_MINIMO_MS, PULSO_TOPE_MS, &v)) {
                snprintf(motivo, tam, "pulso_ms_fuera_de_rango");
                return false;
            }
            nueva.pulso_ms = v;
        } else if (strcmp(p->string, "aviso_s") == 0) {
            if (!cJSON_IsNumber(p) || !entero_en_rango_o_cero(p->valuedouble, AVISO_MIN_S, AVISO_MAX_S, &v)) {
                snprintf(motivo, tam, "aviso_s_fuera_de_rango");
                return false;
            }
            nueva.aviso_s = v;
        } else {
            snprintf(motivo, tam, "parametro_desconocido");
            return false;
        }
    }
    if (!config_guardar(&nueva)) {
        snprintf(motivo, tam, "error_de_nvs");
        return false;
    }
    xSemaphoreTake(lock_cfg, portMAX_DELAY);
    cfg = nueva;
    xSemaphoreGive(lock_cfg);
    ESP_LOGI(TAG, "Config: intervalo %d s, pulso %d ms, aviso %d s", (int)nueva.intervalo_s,
             (int)nueva.pulso_ms, (int)nueva.aviso_s);
    medir_ya = true;                                     /* el intervalo nuevo rige enseguida */
    return true;
}

static void describir_config(cJSON *config)
{
    config_locker_t k = cfg_leer();
    int abiertas[CANT_LOCKERS];
    xSemaphoreTake(lock_locker, portMAX_DELAY);
    for (int i = 0; i < CANT_LOCKERS; i++) abiertas[i] = gabinete.l[i].abierta ? 1 : 0;
    xSemaphoreGive(lock_locker);
    cJSON_AddNumberToObject(config, "intervalo_s", k.intervalo_s);
    cJSON_AddNumberToObject(config, "pulso_ms", k.pulso_ms);
    cJSON_AddNumberToObject(config, "aviso_s", k.aviso_s);
    /* de solo lectura (§3.2): [0,1,0,0] */
    cJSON_AddItemToObject(config, "abiertas", cJSON_CreateIntArray(abiertas, CANT_LOCKERS));
}

/* ============================== Comandos ============================== */
/* Corre en la tarea de nodo_mesh: no se bloquea. El pulso lo arranca la tarea del locker en la
   vuelta siguiente (10 ms). */
static bool ejecutar_comando(const char *comando, const cJSON *params, bool local, char *motivo, size_t tam)
{
    /* El botón del concentrador es del aula: no puede tocar un locker */
    if (local && strcmp(comando, "estado") != 0) {
        snprintf(motivo, tam, "orden_local_no_admitida");
        return false;
    }
    if (strcmp(comando, "estado") == 0) {
        medir_ya = true;
        nodo_mesh_reportar();
        return true;
    }
    if (strcmp(comando, "abrir") == 0) {
#if CONFIG_LOCKER_ABRIR_REMOTO
        const cJSON *pu = cJSON_GetObjectItemCaseSensitive(params, "puerta");
        int puerta;
        if (!puerta_valida(cJSON_IsNumber(pu), cJSON_IsNumber(pu) ? pu->valuedouble : 0, &puerta)) {
            snprintf(motivo, tam, "puerta_invalida");
            return false;
        }
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, "vence");
        bool tiene = cJSON_IsNumber(v);
        orden_estado_t o = orden_vigente(tiene, tiene ? (int64_t)v->valuedouble : 0, (int64_t)time(NULL));
        if (o != ORDEN_OK) {
            snprintf(motivo, tam, "%s", orden_motivo(o));
            return false;
        }
        xSemaphoreTake(lock_locker, portMAX_DELAY);
        abrir_resultado_t r = gabinete_abrir(&gabinete, ahora_ms(), puerta);
        xSemaphoreGive(lock_locker);
        if (r != ABRIR_OK) {
            /* un pulso en curso en el gabinete (no se extiende ni se superpone) o el límite por
               minuto de esa cerradura (sale además limite_pulsos) */
            snprintf(motivo, tam, "ocupada");
            return false;
        }
        ESP_LOGI(TAG, "Orden de abrir el locker %d aceptada", puerta);
        return true;
#else
        (void)params;
        snprintf(motivo, tam, "abrir_deshabilitado");      /* ADR-009, decisión 8 */
        return false;
#endif
    }
    snprintf(motivo, tam, "comando_desconocido");
    return false;
}

/* ============================== Mediciones y acciones ============================== */
static void medir(const bool abiertas[CANT_LOCKERS])
{
    cJSON *v = cJSON_CreateObject();
    if (v == NULL) return;
    char campo[16];
    for (int i = 0; i < CANT_LOCKERS; i++) {
        snprintf(campo, sizeof(campo), "puerta_%d", i + 1);
        cJSON_AddNumberToObject(v, campo, abiertas[i] ? 1 : 0);
    }
    for (int i = 0; i < CANT_LOCKERS; i++) {
        snprintf(campo, sizeof(campo), "cerradura_%d", i + 1);
        cJSON_AddNumberToObject(v, campo, salida_cerradura_energizada(i) ? 1 : 0);
    }
    if (!nodo_mesh_medicion(v)) {
        ESP_LOGW(TAG, "La medición no entró en la cola");
    }
    cJSON_Delete(v);
}

static void alerta(const accion_t *a)
{
    char texto[64];
    snprintf(texto, sizeof(texto), "%s (locker %d)", a->mensaje, a->puerta);
    cJSON *d = cJSON_CreateObject();
    if (d != NULL) cJSON_AddNumberToObject(d, "puerta", a->puerta);
    ESP_LOGW(TAG, "ALERTA %s (%s): %s", a->alerta, severidad_texto(a->severidad), texto);
    nodo_mesh_alerta(a->alerta, severidad_texto(a->severidad), texto, d);
    cJSON_Delete(d);
}

/* Ejecuta, en el orden en que la lógica las dejó, las acciones pendientes. Una intrusion se
   publica antes que la medición de su mismo cambio. */
static void ejecutar_acciones(void)
{
    accion_t a;
    for (;;) {
        xSemaphoreTake(lock_locker, portMAX_DELAY);
        bool hay = gabinete_sacar(&gabinete, &a);
        xSemaphoreGive(lock_locker);
        if (!hay) return;
        switch (a.tipo) {
        case ACC_PULSO:
            salida_cerradura_energizar(a.puerta - 1, (uint32_t)a.valor);
            ESP_LOGI(TAG, "Cerradura %d energizada %d ms", a.puerta, (int)a.valor);
            break;
        case ACC_CORTAR:
            salida_cerradura_soltar(a.puerta - 1);
            ESP_LOGI(TAG, "Cerradura %d trabada", a.puerta);
            break;
        case ACC_ALERTA:
            alerta(&a);
            break;
        case ACC_MEDIR:
            medir_ya = true;
            break;
        }
    }
}

static bool leer_abierta(int i)
{
    return gpio_get_level(PIN_MICROSWITCH[i]) == 1;      /* switch libre o cable cortado */
}

/* ============================== Tarea del gabinete ============================== */
static antirrebote_t sensor[CANT_LOCKERS];

static void tarea_locker(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    bool abiertas[CANT_LOCKERS];
    int64_t ultima = INT64_MIN / 2;                      /* la primera medición sale enseguida */

    for (;;) {
        esp_task_wdt_reset();
        int64_t ahora = ahora_ms();
        config_locker_t k = cfg_leer();

        xSemaphoreTake(lock_locker, portMAX_DELAY);
        gabinete.pulso_ms = k.pulso_ms;
        gabinete.aviso_s = k.aviso_s;
        for (int i = 0; i < CANT_LOCKERS; i++) {
            if (antirrebote_paso(&sensor[i], leer_abierta(i), ahora)) {
                gabinete_sensor(&gabinete, i, ahora, sensor[i].estable);
            }
        }
        gabinete_tick(&gabinete, ahora);
        for (int i = 0; i < CANT_LOCKERS; i++) abiertas[i] = gabinete.l[i].abierta;
        xSemaphoreGive(lock_locker);

        ejecutar_acciones();

        if (medir_ya || (ahora - ultima) >= (int64_t)k.intervalo_s * 1000) {
            medir_ya = false;
            ultima = ahora;
            medir(abiertas);
        }
        vTaskDelay(pdMS_TO_TICKS(PERIODO_LOGICA_MS));
    }
}

/* ============================== LED de estado ============================== */
/* fijo = en la malla con aula · 1 Hz = en la malla sin aula · 4 Hz = buscando la malla */
static void led(bool encendido)
{
    gpio_set_level(PIN_LED, encendido ? 0 : 1);          /* activo en bajo */
}

static void tarea_led(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(125));
        n++;
        if (!nodo_mesh_conectada()) {
            led(n % 2);
        } else if (nodo_mesh_aula()[0] == '\0') {
            led((n / 4) % 2);
        } else {
            led(true);
        }
    }
}

/* ============================== Arranque ============================== */
void app_main(void)
{
    /* 1. Primero las cerraduras: los cuatro pines en 0 antes que cualquier otra cosa */
    salida_cerradura_reposo_inicial();
    salida_cerradura_iniciar();

    uint64_t mascara = 0;
    for (int i = 0; i < CANT_LOCKERS; i++) mascara |= 1ULL << PIN_MICROSWITCH[i];
    gpio_config_t sw = {
        .pin_bit_mask = mascara,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sw));
    gpio_reset_pin(PIN_LED);
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    led(false);

    /* 2. NVS (la usan nodo_mesh y la configuración del gabinete) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    config_cargar();

    lock_cfg = xSemaphoreCreateMutex();
    lock_locker = xSemaphoreCreateMutex();
    configASSERT(lock_cfg && lock_locker);

    /* 3. La lógica ANTES que la malla: un abrir que llegue enseguida ya encuentra pulso_ms y el
       estado de cada locker. Los que arrancan abiertos dejan arranque_puerta_abierta en la cola,
       que sale cuando arranca la tarea. */
    vTaskDelay(pdMS_TO_TICKS(100));                      /* que las pull-up se estabilicen */
    bool abiertas[CANT_LOCKERS];
    int64_t ahora = ahora_ms();
    for (int i = 0; i < CANT_LOCKERS; i++) {
        abiertas[i] = leer_abierta(i);
        antirrebote_iniciar(&sensor[i], abiertas[i], ahora);
    }
    gabinete_iniciar(&gabinete, ahora, abiertas, cfg.pulso_ms, cfg.aviso_s, CORTAR_AL_ABRIR);

    ESP_LOGI(TAG, "Gabinete de %d lockers: pulso %d ms (tope %d), aviso %d s, intervalo %d s",
             CANT_LOCKERS, (int)cfg.pulso_ms, PULSO_TOPE_MS, (int)cfg.aviso_s, (int)cfg.intervalo_s);
    ESP_LOGI(TAG, "Apertura remota %s, cortar al abrir %s",
             ABRIR_REMOTO ? "HABILITADA (solo banco)" : "deshabilitada",
             CORTAR_AL_ABRIR ? "sí" : "no");

    /* 4. La malla */
    nodo_mesh_config_t mcfg = {
        .mesh_id = {MESH_ID_BYTES},
        .mesh_clave = MESH_AP_PASS,
        .potencia_tx = POTENCIA_TX,
        .alimentacion = "red",
        .aplicar_config = aplicar_config,
        .describir_config = describir_config,
        .ejecutar_comando = ejecutar_comando,
    };
    ESP_ERROR_CHECK(nodo_mesh_iniciar(&mcfg));

    /* 5. El gabinete y el LED */
    xTaskCreate(tarea_locker, "gabinete", 4096, NULL, 5, NULL);
    xTaskCreate(tarea_led, "led", 2048, NULL, 2, NULL);
}
