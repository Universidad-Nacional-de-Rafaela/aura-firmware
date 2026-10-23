/*
 * E1-PB-LECA-LUZ01: control de iluminacion del aula del LabECA (hoja de la mesh
 * de AURA sobre la biblioteca nodo_mesh). ESP-IDF puro, ESP32-C3 SuperMini.
 *
 * Mide presencia (radar LD2420 por UART1), luz (dos BH1750 por I2C: ventana y
 * aula), temperatura y humedad (SHT30), y maneja el rele de la luz del aula con
 * la regla de main/regla_luz.h:
 *
 *   sin gente -> reposo, luz apagada;
 *   con gente -> BAJO < umbral_bajo_lx (20 lx por defecto) -> prender,
 *                ALTO > umbral_alto_lx (40 lx) en los dos  -> apagar,
 *                medio -> mantener el estado (evita parpadeo).
 *   Un sensor de luz en falla lo decide el otro; si fallan los dos, mantiene.
 *   Un cambio se sostiene 2 s (debounce) antes de conmutar el rele.
 *
 * Medicion (contrato 3.1), solo con campos validos (una sonda en falla no manda
 * centinela: su campo desaparece y va la alerta "sensor" via nodo_mesh_sonda):
 *   {"presencia": 0|1, "luz_ventana_lx", "luz_aula_lx",
 *    "temp_c", "humedad_pct", "rele_canal1"}
 *
 * set_config (3.3): intervalo_s (5..86400), umbral_bajo_lx y umbral_alto_lx
 * (1..20000, alto > bajo), delay_off_s (0..3600). Todo o nada: primero se
 * valida TODO y recien despues se aplica y se guarda en NVS. "aula" lo maneja
 * la biblioteca y aca se ignora.
 *
 * Sin MAC, ids ni aula en el codigo: la identidad es la placa (hw_id = mac-...)
 * y el aula la asigna AURA con set_config {"aula": ...}. Todas las hojas del
 * dispositivo llevan este MISMO binario.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "cJSON.h"
#include "nodo_mesh.h"

#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error "Este firmware es para placa ESP32-C3 SuperMini: elegi el target esp32c3"
#endif

#if __has_include("config_local.h")
#include "config_local.h"
#else
#error "Falta main/config_local.h: copia config_local.ejemplo.h como config_local.h y completa el ID y la clave de la mesh"
#endif

#include "aura_tiempo.h"
#include "regla_luz.h"
#include "bh1750.h"
#include "sht31.h"
#include "ld2420.h"

// ===== Pines (defaults de la placa; se pueden sobre-escribir en config_local.h) =====
#ifndef LD2420_RX
#define LD2420_RX 7
#endif
#ifndef LD2420_TX
#define LD2420_TX 6
#endif
#ifndef LD2420_BAUD
#define LD2420_BAUD 115200
#endif
#ifndef LED_PRESENCIA_PIN
#define LED_PRESENCIA_PIN 1
#endif
#ifndef LED_PRESENCIA_INVERTIDO
#define LED_PRESENCIA_INVERTIDO false
#endif
#ifndef I2C_SDA
#define I2C_SDA 4
#endif
#ifndef I2C_SCL
#define I2C_SCL 5
#endif
#ifndef RELE_PIN
#define RELE_PIN 0
#endif
#ifndef RELE_ACTIVO_EN_LOW
#define RELE_ACTIVO_EN_LOW true
#endif

// Potencia en pasos de 0,25 dBm. La C3 SuperMini tiene la antena mal adaptada:
// 60 = 15 dBm (ver GUIA_NODOS_HOJA.md, seccion 6). No subir de 19,5 dBm.
#define POTENCIA_TX 60

// ===== Logica de la luz =====
#define DEBOUNCE_MS 2000
#define USAR_PRESENCIA 1

// ===== Ventana de distancia del radar =====
#define DIST_MIN_CM 20
#define DIST_MAX_CM 400
#define DIST_HIST_CM 20

#define CAMPO_PRESENCIA "presencia"

// ===== Configuracion remota (set_config) =====
#define INTERVALO_MIN_S 5
#define INTERVALO_MAX_S 86400
#define UMBRAL_MIN_LX 1
#define UMBRAL_MAX_LX 20000
#define DELAY_OFF_MAX_S 3600

static const uint32_t LUX_BAJO_DEFECTO = 20;
static const uint32_t LUX_ALTO_DEFECTO = 40;
static const uint32_t INTERVALO_DEFECTO_S = 60;
static const uint32_t DELAY_OFF_DEFECTO_S = 10;

static const char *TAG = AURA_TAG;

static uint32_t intervalo_s = INTERVALO_DEFECTO_S;
static uint32_t umbral_bajo_lx = LUX_BAJO_DEFECTO;
static uint32_t umbral_alto_lx = LUX_ALTO_DEFECTO;
static uint32_t delay_off_s = DELAY_OFF_DEFECTO_S;

// ===== Estado =====
static nvs_handle_t config_nvs;
static bool config_nvs_ok = false;

static i2c_master_bus_handle_t i2c_bus;
static bh1750_t bh_ventana;
static bh1750_t bh_aula;
static sht31_t sht;
static ld2420_t radar;

static bool bh_ventana_ok = false, bh_aula_ok = false, sht_ok = false;
static float lux_ventana = -1.0f;
static float lux_aula = -1.0f;
static float temp_c = NAN;
static float humedad_pct = NAN;

static bool luz_encendida = false;
static bool candidato = false;
static uint32_t desde_candidato = 0;

static bool en_ventana = false;
static bool presencia = false;
static int distancia_cm = -1;
static uint32_t ultima_presencia_ms = 0;
static bool sistema_activo = false;

static uint32_t ultima_lectura = 0;
static uint32_t ultima_muestra = 0;
static bool primera_muestra = true;
static bool primera_lectura = true;   /* nada se publica antes de leer los sensores */
static bool publicar_ya = true;
static bool presencia_publicada = false;

/* Guia, regla 8: con float, cJSON escribe 24.100000381 y la medicion completa
   pasa de los 160 bytes de values (la biblioteca la descarta). Un decimal alcanza. */
static double redondear1(float v)
{
    return round((double)v * 10.0) / 10.0;
}

// ===== Rele y LED =====
static void escribir_rele(bool encender)
{
    if (RELE_PIN < 0) return;
    int nivel = RELE_ACTIVO_EN_LOW ? !encender : encender;
    gpio_set_level((gpio_num_t)RELE_PIN, nivel ? 1 : 0);
}

static void led_presencia(bool prendir)
{
    if (LED_PRESENCIA_PIN < 0) return;
    int nivel = LED_PRESENCIA_INVERTIDO ? !prendir : prendir;
    gpio_set_level((gpio_num_t)LED_PRESENCIA_PIN, nivel ? 1 : 0);
}

static void configurar_gpio_salida(int pin, bool nivel_inicial)
{
    if (pin < 0) return;
    gpio_config_t g = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&g);
    gpio_set_level((gpio_num_t)pin, nivel_inicial ? 1 : 0);
}

// Logica del rele (regla_luz.h) + anti-parpadeo de 2 s.
static void controlar_rele(void)
{
    bool deseado = regla_deseado(sistema_activo, bh_ventana_ok, lux_ventana,
                                 bh_aula_ok, lux_aula, luz_encendida,
                                 umbral_bajo_lx, umbral_alto_lx);
    if (deseado == luz_encendida) {
        candidato = deseado;
    } else if (deseado != candidato) {
        candidato = deseado;
        desde_candidato = aura_millis();
    } else if (aura_millis() - desde_candidato >= DEBOUNCE_MS) {
        luz_encendida = deseado;
        escribir_rele(luz_encendida);
        publicar_ya = true;
        ESP_LOGI(TAG, ">> RELE %s, luz %s", luz_encendida ? "ON" : "off",
                 luz_encendida ? "prendida" : "apagada");
    }
}

// ===== Sensores =====
static float leer_bh(bh1750_t *s, bool *ok, uint8_t dir)
{
    if (!*ok) {
        *ok = bh1750_init(s, i2c_bus, dir);
        if (!*ok) return -1.0f;
    }
    float v = bh1750_leer_lux(s);
    if (v < 0) *ok = false;
    return v;
}

static void leer_sht(void)
{
    if (!sht_ok) {
        sht_ok = sht31_init(&sht, i2c_bus, 0x44);
        if (!sht_ok) { temp_c = NAN; humedad_pct = NAN; return; }
    }
    float t, h;
    if (!sht31_leer(&sht, &t, &h)) { sht_ok = false; temp_c = NAN; humedad_pct = NAN; return; }
    temp_c = t;
    humedad_pct = h;
}

// ===== Configuracion remota (callbacks de nodo_mesh) =====
static void set_motivo(char *motivo, size_t cap, const char *texto)
{
    snprintf(motivo, cap, "%s", texto);
}

/* Valida que p sea un entero entre min y max ANTES de convertirlo: un negativo
   o un 30.7 no se pueden castear a uint32_t sin perder o inventar el valor. */
static bool leer_entero(const cJSON *p, const char *nombre, uint32_t min, uint32_t max,
                        uint32_t *destino, char *motivo, size_t cap)
{
    if (!cJSON_IsNumber(p) || p->valuedouble != floor(p->valuedouble)) {
        snprintf(motivo, cap, "%s_no_es_entero", nombre);
        return false;
    }
    if (p->valuedouble < (double)min || p->valuedouble > (double)max) {
        snprintf(motivo, cap, "%s_fuera_de_rango", nombre);
        return false;
    }
    *destino = (uint32_t)p->valuedouble;
    return true;
}

static bool aplicar_config(const cJSON *params, char *motivo, size_t cap)
{
    /* Primero se valida TODO; solo despues se aplica (contrato 3.3). "aula" la
       maneja la biblioteca: aca se ignora. */
    uint32_t nuevo_intervalo = intervalo_s;
    uint32_t nuevo_bajo = umbral_bajo_lx;
    uint32_t nuevo_alto = umbral_alto_lx;
    uint32_t nuevo_delay = delay_off_s;

    const cJSON *p;
    cJSON_ArrayForEach(p, params)
    {
        const char *k = p->string;
        if (k == NULL) { set_motivo(motivo, cap, "parametro_sin_nombre"); return false; }
        if (strcmp(k, "aula") == 0) continue;   /* lo guarda la biblioteca */
        if (strcmp(k, "intervalo_s") == 0) {
            if (!leer_entero(p, "intervalo_s", INTERVALO_MIN_S, INTERVALO_MAX_S, &nuevo_intervalo, motivo, cap)) return false;
        } else if (strcmp(k, "umbral_bajo_lx") == 0) {
            if (!leer_entero(p, "umbral_bajo_lx", UMBRAL_MIN_LX, UMBRAL_MAX_LX, &nuevo_bajo, motivo, cap)) return false;
        } else if (strcmp(k, "umbral_alto_lx") == 0) {
            if (!leer_entero(p, "umbral_alto_lx", UMBRAL_MIN_LX, UMBRAL_MAX_LX, &nuevo_alto, motivo, cap)) return false;
        } else if (strcmp(k, "delay_off_s") == 0) {
            if (!leer_entero(p, "delay_off_s", 0, DELAY_OFF_MAX_S, &nuevo_delay, motivo, cap)) return false;
        } else {
            snprintf(motivo, cap, "parametro_desconocido:%s", k);
            return false;
        }
    }

    if (nuevo_bajo >= nuevo_alto) { set_motivo(motivo, cap, "umbrales_invertidos"); return false; }

    intervalo_s = nuevo_intervalo;
    umbral_bajo_lx = nuevo_bajo;
    umbral_alto_lx = nuevo_alto;
    delay_off_s = nuevo_delay;
    if (config_nvs_ok) {
        nvs_set_u32(config_nvs, "intervalo_s", intervalo_s);
        nvs_set_u32(config_nvs, "umbral_bajo", umbral_bajo_lx);
        nvs_set_u32(config_nvs, "umbral_alto", umbral_alto_lx);
        nvs_set_u32(config_nvs, "delay_off", delay_off_s);
        nvs_commit(config_nvs);
    }
    ESP_LOGI(TAG, "config aplicada: intervalo_s=%lu umbral_bajo_lx=%lu umbral_alto_lx=%lu delay_off_s=%lu",
             (unsigned long)intervalo_s, (unsigned long)umbral_bajo_lx,
             (unsigned long)umbral_alto_lx, (unsigned long)delay_off_s);
    publicar_ya = true;
    return true;
}

static void describir_config(cJSON *out)
{
    cJSON_AddNumberToObject(out, "intervalo_s", intervalo_s);
    cJSON_AddNumberToObject(out, "umbral_bajo_lx", umbral_bajo_lx);
    cJSON_AddNumberToObject(out, "umbral_alto_lx", umbral_alto_lx);
    cJSON_AddNumberToObject(out, "delay_off_s", delay_off_s);
}

// ===== Diagnostico del radar =====
static bool diag_pendiente = true;
static uint32_t diag_ultimo_ms = 0;

static void diagnosticar_radar(void)
{
    if (!diag_pendiente || aura_millis() - diag_ultimo_ms < 2000) return;
    diag_ultimo_ms = aura_millis();

    uint32_t validos = ld2420_frames_validos(&radar);
    if (validos > 0) {
        ESP_LOGI(TAG, "[RADAR] diag OK: %lu reportes validos, distancia %u cm (el sensor responde)",
                 (unsigned long)validos, (unsigned)ld2420_distancia_cm(&radar));
        diag_pendiente = false;
        return;
    }
    if (aura_millis() >= 20000) {
        if (ld2420_frames_rechazados(&radar) > 0) {
            ESP_LOGW(TAG, "[RADAR] diag: llegaron %lu bytes que no se entienden -> revisar cableado "
                     "(OT1->GPIO%d, RX->GPIO%d, GND comun)",
                     (unsigned long)ld2420_frames_rechazados(&radar), LD2420_RX, LD2420_TX);
        } else {
            ESP_LOGW(TAG, "[RADAR] diag: no llego nada del sensor -> 1) el TX del modulo es OT1 (no OT2), "
                     "2) alimentar con 3V3, 3) probar LD2420_BAUD 256000 (pines OT1->GPIO%d, RX->GPIO%d)",
                     LD2420_RX, LD2420_TX);
        }
        diag_pendiente = false;
        return;
    }
    ESP_LOGI(TAG, "[RADAR] diag: esperando datos del sensor (%lu ms)...", (unsigned long)aura_millis());
}

// ===== Arranque =====
static void cargar_config_nvs(void)
{
    uint32_t v;
    if (config_nvs_ok) {
        if (nvs_get_u32(config_nvs, "intervalo_s", &v) == ESP_OK) intervalo_s = v;
        if (nvs_get_u32(config_nvs, "umbral_bajo", &v) == ESP_OK) umbral_bajo_lx = v;
        if (nvs_get_u32(config_nvs, "umbral_alto", &v) == ESP_OK) umbral_alto_lx = v;
        if (nvs_get_u32(config_nvs, "delay_off", &v) == ESP_OK) delay_off_s = v;
    }
    if (intervalo_s < INTERVALO_MIN_S || intervalo_s > INTERVALO_MAX_S) intervalo_s = INTERVALO_DEFECTO_S;
    if (umbral_bajo_lx < UMBRAL_MIN_LX || umbral_bajo_lx > UMBRAL_MAX_LX) umbral_bajo_lx = LUX_BAJO_DEFECTO;
    if (umbral_alto_lx < UMBRAL_MIN_LX || umbral_alto_lx > UMBRAL_MAX_LX) umbral_alto_lx = LUX_ALTO_DEFECTO;
    if (umbral_bajo_lx >= umbral_alto_lx) {
        umbral_bajo_lx = LUX_BAJO_DEFECTO;
        umbral_alto_lx = LUX_ALTO_DEFECTO;
    }
    if (delay_off_s > DELAY_OFF_MAX_S) delay_off_s = DELAY_OFF_DEFECTO_S;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "=== E1-PB-LECA-LUZ01 (AURA, hoja de la mesh, ESP-IDF) ===");

    // NVS: configuracion del dispositivo + la de la biblioteca (la inita ella).
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    config_nvs_ok = nvs_open("luz01", NVS_READWRITE, &config_nvs) == ESP_OK;
    cargar_config_nvs();

    // Rele y LED: nivel ANTES de pasar a salida, arrancan apagados.
    if (RELE_PIN >= 0) configurar_gpio_salida(RELE_PIN, RELE_ACTIVO_EN_LOW ? 1 : 0);
    if (LED_PRESENCIA_PIN >= 0)
        configurar_gpio_salida(LED_PRESENCIA_PIN, LED_PRESENCIA_INVERTIDO ? 1 : 0);
    luz_encendida = false;
    led_presencia(false);

    // I2C: BH1750 ventana (0x23), BH1750 aula (0x5C) y SHT30 (0x44).
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)I2C_SDA,
        .scl_io_num = (gpio_num_t)I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    if (i2c_new_master_bus(&bus_cfg, &i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "!! no arranco el bus I2C");
    } else {
        bh_ventana_ok = bh1750_init(&bh_ventana, i2c_bus, 0x23);
        bh_aula_ok = bh1750_init(&bh_aula, i2c_bus, 0x5C);
        sht_ok = sht31_init(&sht, i2c_bus, 0x44);
    }
    ESP_LOGI(TAG, "BH1750 ventana(0x23): %s, aula(0x5C): %s, SHT30(0x44): %s",
             bh_ventana_ok ? "OK" : "FALLA", bh_aula_ok ? "OK" : "FALLA", sht_ok ? "OK" : "FALLA");

    // LD2420 por UART1 (3.3 V logicos). Sin sensor la sonda queda en falla y no
    // se publica presencia: nunca manda un valor inventado.
    if (!ld2420_init(&radar, LD2420_RX, LD2420_TX, LD2420_BAUD)) {
        ESP_LOGW(TAG, "LD2420: no arranco (error %d): revisar cableado (OT1->GPIO%d, RX->GPIO%d) "
                 "o baudios (%u)", ld2420_ultimo_error(&radar), LD2420_RX, LD2420_TX, (unsigned)LD2420_BAUD);
    }

    // Union a la mesh (la biblioteca hace el resto: aula, cola, hora, eventos).
    nodo_mesh_config_t cfg = {
        .mesh_id = {MESH_ID_BYTES},
        .mesh_clave = MESH_AP_PASS,
        .potencia_tx = POTENCIA_TX,
        .alimentacion = "red",
        .aplicar_config = aplicar_config,
        .describir_config = describir_config,
        .ejecutar_comando = NULL,
    };
    ESP_ERROR_CHECK(nodo_mesh_iniciar(&cfg));

    ESP_LOGI(TAG, "listo: BAJO <%lu lx, ALTO >%lu lx, ventana %u..%u cm (sale en 0..%u), "
             "reposo %lu s, intervalo %lu s",
             (unsigned long)umbral_bajo_lx, (unsigned long)umbral_alto_lx,
             (unsigned)DIST_MIN_CM, (unsigned)DIST_MAX_CM,
             (unsigned)(DIST_MAX_CM + DIST_HIST_CM),
             (unsigned long)delay_off_s, (unsigned long)intervalo_s);
    ESP_LOGI(TAG, "%s", USAR_PRESENCIA ? "Presencia: ACTIVADA (sin gente el sistema queda en reposo)"
                                       : "Presencia: DESACTIVADA (modo prueba)");
    ultima_presencia_ms = aura_millis();
    sistema_activo = true;

    // ===== Bucle principal =====
    bool movimiento_anterior = false;
    bool ventana_anterior = false;
    for (;;) {
        ld2420_update(&radar);
        diagnosticar_radar();

        bool vencio_lectura = primera_lectura || aura_millis() - ultima_lectura >= 1000;
        if (vencio_lectura) {
            primera_lectura = false;
            ultima_lectura = aura_millis();
            lux_ventana = leer_bh(&bh_ventana, &bh_ventana_ok, 0x23);
            lux_aula = leer_bh(&bh_aula, &bh_aula_ok, 0x5C);
            leer_sht();
        }

        // ===== Presencia =====
        bool stale = ld2420_is_stale(&radar);
        uint16_t distancia = ld2420_distancia_cm(&radar);
        bool movimiento = ld2420_movimiento(&radar);

        int d = (int)distancia;
        if (d >= (int)DIST_MIN_CM && d <= (int)DIST_MAX_CM) {
            en_ventana = true;
        } else if (d < (int)DIST_MIN_CM - (int)DIST_HIST_CM ||
                   d > (int)DIST_MAX_CM + (int)DIST_HIST_CM) {
            en_ventana = false;
        }

        distancia_cm = stale ? -1 : d;
        presencia = !stale && movimiento && en_ventana;
        if (presencia) ultima_presencia_ms = aura_millis();

        led_presencia(!stale && en_ventana);

        if (!stale && (movimiento != movimiento_anterior || en_ventana != ventana_anterior)) {
            movimiento_anterior = movimiento;
            ventana_anterior = en_ventana;
            ESP_LOGI(TAG, "[RADAR] %s, distancia %u cm (entra %u..%u, sale 0..%u) -> %s",
                     movimiento ? "movimiento" : "sin movimiento", (unsigned)distancia,
                     (unsigned)DIST_MIN_CM, (unsigned)DIST_MAX_CM,
                     (unsigned)(DIST_MAX_CM + DIST_HIST_CM),
                     presencia ? "PRESENCIA" : "no cuenta");
        }

        // Reposo: sin presencia durante delay_off_s el sistema se apaga.
        bool activo_antes = sistema_activo;
        sistema_activo = !USAR_PRESENCIA ||
                         aura_millis() - ultima_presencia_ms < delay_off_s * 1000UL;
        if (sistema_activo != activo_antes) {
            ESP_LOGI(TAG, "%s", sistema_activo ? ">> Hay gente: sistema ACTIVO"
                                               : ">> Sin gente: sistema en REPOSO");
        }

        controlar_rele();

        if (vencio_lectura) {
            char la[16], lv[16], lt[16], lh[16];
            if (lux_aula < 0) snprintf(la, sizeof(la), "FALLA"); else snprintf(la, sizeof(la), "%.0f lx", lux_aula);
            if (lux_ventana < 0) snprintf(lv, sizeof(lv), "FALLA"); else snprintf(lv, sizeof(lv), "%.0f lx", lux_ventana);
            if (isnan(temp_c)) snprintf(lt, sizeof(lt), "FALLA"); else snprintf(lt, sizeof(lt), "%.1f C", temp_c);
            if (isnan(humedad_pct)) snprintf(lh, sizeof(lh), "FALLA"); else snprintf(lh, sizeof(lh), "%.0f %%", humedad_pct);
            ESP_LOGI(TAG, "Aula: %s | Ventana: %s | T: %s | HR: %s | Presencia: %s%s%s | cola %lu",
                     la, lv, lt, lh, presencia ? "SI" : "no",
                     distancia_cm >= 0 ? " a" : "",
                     en_ventana ? " (en ventana)" : " (fuera de ventana)",
                     (unsigned long)nodo_mesh_pendientes());
        }

        // ===== Sondas (alerta "sensor" solo cuando cambia, contrato 3.5) =====
        nodo_mesh_sonda(CAMPO_PRESENCIA, !stale, "sin_respuesta");
        nodo_mesh_sonda("luz_ventana_lx", bh_ventana_ok, "sin_respuesta");
        nodo_mesh_sonda("luz_aula_lx", bh_aula_ok, "sin_respuesta");
        nodo_mesh_sonda("temp_c", sht_ok, "sin_respuesta");
        nodo_mesh_sonda("humedad_pct", sht_ok, "sin_respuesta");

        // ===== Medicion =====
        if (!stale && presencia != presencia_publicada) publicar_ya = true;
        bool vencio = primera_muestra ||
                      aura_millis() - ultima_muestra >= intervalo_s * 1000UL;
        if (vencio || publicar_ya) {
            cJSON *values = cJSON_CreateObject();
            if (!stale) cJSON_AddNumberToObject(values, CAMPO_PRESENCIA, presencia ? 1 : 0);
            if (bh_ventana_ok) cJSON_AddNumberToObject(values, "luz_ventana_lx", redondear1(lux_ventana));
            if (bh_aula_ok) cJSON_AddNumberToObject(values, "luz_aula_lx", redondear1(lux_aula));
            if (sht_ok) {
                cJSON_AddNumberToObject(values, "temp_c", redondear1(temp_c));
                cJSON_AddNumberToObject(values, "humedad_pct", redondear1(humedad_pct));
            }
            if (RELE_PIN >= 0) cJSON_AddNumberToObject(values, "rele_canal1", luz_encendida ? 1 : 0);
            if (!nodo_mesh_medicion(values)) ESP_LOGW(TAG, "medicion no encolada (JSON vacio o muy grande)");
            cJSON_Delete(values);
            if (!stale) presencia_publicada = presencia;
            publicar_ya = false;
            ultima_muestra = aura_millis();
            primera_muestra = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
