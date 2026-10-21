/*
 * Mesh de AURA - CONCENTRADOR DEL AULA (ESP32-S3, capa 2). Contrato MQTT v4.0.
 *
 * - Se une a la raíz fija (gateway) y acepta hijos: las hojas de su aula se cuelgan de él.
 * - El AULA no está en el firmware: se guarda en NVS y la asigna AURA con
 *   set_config {"aula":"aula1"}. Sin aula no anuncia nada y solo reenvía como cualquier nodo.
 *   Con aula, la anuncia en su beacon (vendor IE con su aula y su MAC): así cada hoja
 *   encuentra a SU concentrador sin MACs en ningún firmware.
 * - Cada dato de una hoja de su aula llega UNA vez: lo muestra ("pantalla"), puede
 *   filtrarlo (filtrar()) y lo reenvía a la raíz dentro de un LOTE con la MAC de la hoja,
 *   juntando lo que llega en ESPERA_LOTE_MS. Nada viaja dos veces.
 * - Lo que AURA le manda a una hoja (comandos, confirmaciones) va directo a la MAC de la
 *   hoja: la mesh lo enruta a través de este concentrador sin que tenga que hacer nada.
 * - Contesta él mismo los pedidos de hora de sus hojas, si ya tiene hora.
 * - "Pantalla": LED RGB + puerto serie. Azul = LED de la hoja prendido; ROJO = hay una hoja del
 *   aula AUSENTE (se desconectó y no volvió en HOJA_AUSENTE_MS, o pasó 3 intervalos sin hablar).
 *   Las hojas del aula se recuerdan en NVS: si el concentrador se reinicia y una no vuelve,
 *   también se marca ausente.
 * - Mando local: el botón BOOT manda {"command":"led","params":{"invertir":true},"origen":"local"}
 *   a la hoja, sin pasar por el gateway ni por MQTT.
 * - MODO ISLA: si pasa ISLA_TRAS_S sin raíz (gateway apagado), se reinicia como raíz de su
 *   propia isla, en el último canal, para que el aula siga andando: las hojas se cuelgan de él,
 *   la pantalla y el botón funcionan, y las mediciones quedan en la cola de cada hoja. Cada
 *   ISLA_REVISAR_S busca al gateway; cuando lo ve, se reinicia en modo normal. (Sin raíz,
 *   ESP-WIFI-MESH no acepta conexiones nuevas: una hoja que se desconecta quedaría afuera.)
 * - Cada INTERVALO_REPORTE_S manda su propio reporte (y cuando la raíz lo pide al volver el
 *   broker): la raíz lo publica en hw/mac-<concentrador>/status. El resultado de un comando
 *   propio se reintenta hasta que la raíz confirma que lo publicó.
 *
 * MESH_ID y MESH_AP_PASS (config_local.h) deben coincidir con el gateway y las hojas.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "cJSON.h"
#include "aura_trama.h"
#include "aura_util.h"

#if __has_include("config_local.h")
#include "config_local.h"
#else
#error "Falta main/config_local.h: copiá config_local.ejemplo.h como config_local.h y completá el ID y la clave de la mesh"
#endif
static const uint8_t MESH_ID[6] = {MESH_ID_BYTES};

#define MESH_MAX_HIJOS          6
#define MESH_MAX_CAPAS          6
/* La API exige un SSID de router, pero el concentrador nunca se conecta al router. Fijo. */
#define ROUTER_FICTICIO         "aura-sin-router"

#define ESPERA_LOTE_MS          50      /* cuánto se juntan datos de hojas antes de mandarlos */
#define INTERVALO_REPORTE_S     60
#define PEDIR_HORA_MS           10000
#define LIMITE_ENVIO_MS         5000
#define FALLOS_PARA_RECONECTAR  3

#define HOJA_AUSENTE_MS         20000   /* desconectada del concentrador tanto -> ausente */
#define GRACIA_ARRANQUE_MS      60000   /* al arrancar, las hojas tardan en colgarse */
#define INTERVALO_HOJA_DEFECTO  60      /* si la hoja todavía no informó su intervalo */
#define ISLA_TRAS_S             60      /* sin raíz tanto tiempo -> modo isla */
#define ISLA_REVISAR_S          30      /* en isla, cada cuánto se busca al gateway en el canal */
#define ISLA_REVISAR_TODO_S     300     /* y cada cuánto en todos los canales (por si cambió) */
#define ROUTER_ISLA             "aura-isla"

#define PIN_BOTON               GPIO_NUM_0     /* botón BOOT */
#define PIN_LED_RGB             38             /* WS2812 de la Waveshare ESP32-S3-DEV-KIT */
#define BRILLO_LED              150

static const char *TAG = "CONCENTRADOR";
static volatile bool conectado = false;
static volatile bool reconecto = false;
/* Modo isla: sobrevive al esp_restart() (no a un corte de luz, que arranca en modo normal) */
#define RTC_ISLA                0x49534C41u    /* "ISLA" */
static RTC_NOINIT_ATTR uint32_t rtc_isla;
static RTC_NOINIT_ATTR uint8_t rtc_canal_isla;
static bool isla = false;
static uint8_t canal_conocido = 0;               /* último canal de la malla (NVS) */
static volatile TickType_t sin_padre_desde = 0;
static volatile int hijos = 0;
static volatile int fallos_envio = 0;
static led_strip_handle_t led_rgb = NULL;
static char aula[AURA_AULA_MAX] = "";
static uint8_t mi_mac[6];
static uint16_t seq = 0;
static SemaphoreHandle_t lock_envio = NULL;      /* seq y esp_mesh_send desde varias tareas */

/* Hojas del aula, aprendidas de sus tramas (no se configura ninguna MAC) */
#define MAX_HOJAS 8
typedef struct {
    uint8_t mac[6];
    TickType_t visto;
    char aula[AURA_AULA_MAX];          /* la que informa en su reporte; "" = todavía no informó */
    uint32_t ts_mostrado;              /* hora del último valor mostrado: no se vuelve atrás */
    bool hay_evento;
    uint16_t seq_evento;               /* último evento mostrado: un reintento no se muestra dos veces */
    uint32_t intervalo_s;              /* el de su reporte: 3 intervalos sin hablar -> ausente */
    TickType_t desconectada_desde;     /* 0 = colgada de este concentrador */
    bool ausente;
} hoja_t;
static hoja_t hojas[MAX_HOJAS];
static int cant_hojas = 0;
static int hoja_led = -1;                        /* -1 = todavía no informó */

/* LOTE que se arma con lo que mandan las hojas */
static aura_lote_t lote;
static TickType_t lote_desde = 0;
static SemaphoreHandle_t lock_lote = NULL;

/* Comandos para este concentrador: los atiende la tarea de estado (puede tener que responder) */
typedef struct {
    uint16_t seq;
    char json[AURA_PAYLOAD_MAX + 1];
} comando_t;
static QueueHandle_t cola_comandos = NULL;

/* Resultado de un comando propio: se reintenta cada 3 s hasta el EVENTO_OK de la raíz */
static aura_trama_t resultado_pendiente;
static volatile bool hay_resultado_pendiente = false;
static TickType_t resultado_enviado_en = 0;
static volatile bool reportar_ya = false;        /* la raíz lo pidió (volvió el broker) */

/* ================================================================== */
/*  Pantalla: LED RGB + puerto serie                                   */
/* ================================================================== */
static bool hay_ausente(void);

/* Rojo si falta una hoja del aula; si no, azul con el LED de la hoja prendido */
static void pantalla_led(void)
{
    if (led_rgb == NULL) {
        return;
    }
    if (hay_ausente()) {
        led_strip_set_pixel(led_rgb, 0, BRILLO_LED, 0, 0);
        led_strip_refresh(led_rgb);
    } else if (hoja_led == 1) {
        led_strip_set_pixel(led_rgb, 0, 0, 0, BRILLO_LED);
        led_strip_refresh(led_rgb);
    } else {
        led_strip_clear(led_rgb);
    }
}

static void led_mostrar(int estado)
{
    (void)estado;
    pantalla_led();
}

static void led_iniciar(void)
{
    led_strip_config_t c = {
        .strip_gpio_num = PIN_LED_RGB,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,   /* este LED usa orden R-G-B */
    };
    led_strip_rmt_config_t rmt = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
    if (led_strip_new_rmt_device(&c, &rmt, &led_rgb) != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo iniciar el LED RGB en GPIO%d", PIN_LED_RGB);
        led_rgb = NULL;
        return;
    }
    led_strip_clear(led_rgb);
}

/* ================================================================== */
/*  Hora                                                               */
/* ================================================================== */
static uint64_t ahora_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return aura_hora_valida((uint32_t)(ms / 1000)) ? ms : 0;
}

static void hora_ajustar(uint64_t ms)
{
    if (!aura_hora_valida((uint32_t)(ms / 1000))) {
        return;
    }
    bool primera = ahora_ms() == 0;
    struct timeval tv = {.tv_sec = (time_t)(ms / 1000), .tv_usec = (suseconds_t)(ms % 1000) * 1000};
    settimeofday(&tv, NULL);
    if (primera) {
        ESP_LOGI(TAG, "Hora recibida del gateway");
    }
}

/* ================================================================== */
/*  Envío                                                              */
/* ================================================================== */
/* destino NULL = la raíz. Con la raíz, vigilante de envío (control de flujo trabado). */
static bool enviar_bytes(const mesh_addr_t *destino, const uint8_t *datos, size_t largo)
{
    if (destino == NULL && (isla || !conectado)) {
        return false;                          /* hacia arriba no hay a quién (las hojas reintentan) */
    }
    mesh_data_t d = {.data = (uint8_t *)datos, .size = (uint16_t)largo, .proto = MESH_PROTO_BIN, .tos = MESH_TOS_P2P};
    esp_err_t err = esp_mesh_send(destino, &d, MESH_DATA_P2P, NULL, 0);
    if (destino != NULL) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Error al enviar a una hoja: %s", esp_err_to_name(err));
        }
        return err == ESP_OK;
    }
    if (err == ESP_OK) {
        fallos_envio = 0;
        return true;
    }
    ESP_LOGW(TAG, "Error al enviar a la raíz: %s", esp_err_to_name(err));
    if (conectado && ++fallos_envio >= FALLOS_PARA_RECONECTAR) {
        ESP_LOGW(TAG, "%d envíos fallidos seguidos: fuerzo la reconexión", fallos_envio);
        fallos_envio = 0;
        esp_mesh_disconnect();
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_mesh_connect();
    }
    return false;
}

/* Trama propia (de este concentrador) */
static bool enviar_trama(const mesh_addr_t *destino, uint8_t tipo, const void *payload, size_t largo)
{
    aura_trama_t t;
    xSemaphoreTake(lock_envio, portMAX_DELAY);
    aura_trama_init(&t, tipo, seq++, payload, largo);
    xSemaphoreGive(lock_envio);
    return enviar_bytes(destino, (const uint8_t *)&t, aura_trama_bytes(&t));
}

static bool enviar_json(const mesh_addr_t *destino, uint8_t tipo, const cJSON *doc)
{
    char *txt = cJSON_PrintUnformatted(doc);
    if (txt == NULL) {
        return false;
    }
    bool ok = strlen(txt) <= AURA_PAYLOAD_MAX && enviar_trama(destino, tipo, txt, strlen(txt));
    cJSON_free(txt);
    return ok;
}

/* Manda a la raíz lo juntado de las hojas, en un solo paquete */
static void lote_mandar(void)
{
    xSemaphoreTake(lock_lote, portMAX_DELAY);
    if (lote.cantidad > 0) {
        if (!enviar_bytes(NULL, lote.buf, lote.largo)) {
            ESP_LOGW(TAG, "Lote de %u tramas no enviado (las hojas reintentan lo que no se confirme)", lote.cantidad);
        }
        aura_lote_vaciar(&lote);
    }
    xSemaphoreGive(lock_lote);
}

static void lote_agregar(const uint8_t mac[6], const uint8_t *trama, size_t largo)
{
    xSemaphoreTake(lock_lote, portMAX_DELAY);
    if (lote.cantidad == 0) {
        lote_desde = xTaskGetTickCount();
    }
    bool ok = aura_lote_agregar(&lote, mac, trama, largo);
    xSemaphoreGive(lock_lote);
    if (!ok) {                                         /* no entra: sale el que hay y va en otro */
        lote_mandar();
        xSemaphoreTake(lock_lote, portMAX_DELAY);
        lote_desde = xTaskGetTickCount();
        aura_lote_agregar(&lote, mac, trama, largo);
        xSemaphoreGive(lock_lote);
    }
}

static void tarea_lote(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (lote.cantidad > 0 && (xTaskGetTickCount() - lote_desde) >= pdMS_TO_TICKS(ESPERA_LOTE_MS)) {
            lote_mandar();
        }
    }
}

/* ================================================================== */
/*  Hojas del aula                                                     */
/* ================================================================== */
static hoja_t *hoja_vista(const uint8_t mac[6])
{
    int i;
    for (i = 0; i < cant_hojas && memcmp(hojas[i].mac, mac, 6) != 0; i++) {
    }
    if (i == cant_hojas) {
        if (cant_hojas == MAX_HOJAS) {
            i = 0;                                     /* lleno: se reemplaza la primera */
        } else {
            cant_hojas++;
        }
        memcpy(hojas[i].mac, mac, 6);
        hojas[i].aula[0] = 0;
        hojas[i].ts_mostrado = 0;
        hojas[i].hay_evento = false;
        hojas[i].intervalo_s = 0;
        hojas[i].desconectada_desde = 0;
        hojas[i].ausente = false;
        char hw[AURA_HW_ID_BYTES];
        aura_hw_id_de_mac(mac, hw);
        ESP_LOGI(TAG, "Hoja del aula: %s", hw);
    }
    hojas[i].visto = xTaskGetTickCount();
    hojas[i].desconectada_desde = 0;           /* si habla, está colgada */
    if (hojas[i].ausente) {
        hojas[i].ausente = false;
        char hw[AURA_HW_ID_BYTES];
        aura_hw_id_de_mac(mac, hw);
        printf("PANTALLA %s | %s VOLVIÓ\n", aula, hw);
        pantalla_led();
    }
    return &hojas[i];
}

static bool hay_ausente(void)
{
    for (int i = 0; i < cant_hojas; i++) {
        if (hojas[i].ausente && (hojas[i].aula[0] == 0 || strcmp(hojas[i].aula, aula) == 0)) {
            return true;
        }
    }
    return false;
}

/* ===== Hojas del aula en NVS (se escribe solo cuando cambia la lista) ===== */
static void hojas_guardar(void)
{
    uint8_t macs[MAX_HOJAS][6];
    int n = 0;
    for (int i = 0; i < cant_hojas; i++) {
        if (strcmp(hojas[i].aula, aula) == 0) {
            memcpy(macs[n++], hojas[i].mac, 6);
        }
    }
    nvs_handle_t h;
    if (nvs_open("concentrador", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "hojas", macs, n * 6);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void hojas_cargar(void)
{
    uint8_t macs[MAX_HOJAS][6];
    size_t tam = sizeof(macs);
    nvs_handle_t h;
    if (nvs_open("concentrador", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, "hojas", macs, &tam) == ESP_OK) {
        for (int k = 0; k < (int)(tam / 6) && cant_hojas < MAX_HOJAS; k++) {
            hoja_t *x = &hojas[cant_hojas++];
            memset(x, 0, sizeof(*x));
            memcpy(x->mac, macs[k], 6);
            strlcpy(x->aula, aula, sizeof(x->aula));
            x->visto = xTaskGetTickCount();
            x->desconectada_desde = xTaskGetTickCount() | 1;  /* hasta que se cuelgue */
        }
        ESP_LOGI(TAG, "Hojas del aula conocidas: %d", (int)(tam / 6));
    }
    nvs_close(h);
}

/* Cada pocos segundos: ¿falta alguna hoja del aula? */
static void revisar_ausentes(void)
{
    TickType_t ahora = xTaskGetTickCount();
    if (ahora < pdMS_TO_TICKS(GRACIA_ARRANQUE_MS)) {
        return;
    }
    bool cambio = false;
    for (int i = 0; i < cant_hojas; i++) {
        hoja_t *x = &hojas[i];
        if (x->ausente || (x->aula[0] && strcmp(x->aula, aula) != 0)) {
            continue;
        }
        uint32_t iv = x->intervalo_s ? x->intervalo_s : INTERVALO_HOJA_DEFECTO;
        const char *motivo = NULL;
        if (x->desconectada_desde && ahora - x->desconectada_desde >= pdMS_TO_TICKS(HOJA_AUSENTE_MS)) {
            motivo = "se desconectó y no volvió";
        } else if (ahora - x->visto >= pdMS_TO_TICKS(3 * iv * 1000)) {
            motivo = "3 intervalos sin datos";
        }
        if (motivo) {
            x->ausente = true;
            cambio = true;
            char hw[AURA_HW_ID_BYTES];
            aura_hw_id_de_mac(x->mac, hw);
            printf("PANTALLA %s | %s AUSENTE (%s)\n", aula, hw, motivo);
        }
    }
    if (cambio) {
        pantalla_led();
    }
}

/* La hoja que habló último (para el botón) */
static bool hoja_ultima(mesh_addr_t *mac)
{
    int mejor = -1;
    for (int i = 0; i < cant_hojas; i++) {
        if (hojas[i].aula[0] && strcmp(hojas[i].aula, aula) != 0) {
            continue;                                  /* el botón solo manda a hojas de esta aula */
        }
        if (mejor < 0 || (int32_t)(hojas[i].visto - hojas[mejor].visto) > 0) {
            mejor = i;
        }
    }
    if (mejor >= 0) {
        memcpy(mac->addr, hojas[mejor].mac, 6);
    }
    return mejor >= 0;
}

/* El aula que informa la hoja en su reporte: así no se muestra una hoja de otra aula
   que quedó colgada de este concentrador (por ejemplo, justo después de cambiarle el aula). */
static void aprender_aula(hoja_t *h, const aura_trama_t *t)
{
    if (t->tipo != AURA_TIPO_REPORTE) {
        return;
    }
    char txt[AURA_PAYLOAD_MAX + 1];
    aura_payload_texto(t, txt, sizeof(txt));
    cJSON *d = cJSON_Parse(txt);
    const cJSON *config = cJSON_GetObjectItemCaseSensitive(d, "config");
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(config, "aula");
    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(config, "intervalo_s");
    bool era_mia = strcmp(h->aula, aula) == 0;
    strlcpy(h->aula, cJSON_IsString(a) ? a->valuestring : "-", sizeof(h->aula));
    if (cJSON_IsNumber(iv) && iv->valuedouble > 0) {
        h->intervalo_s = (uint32_t)iv->valuedouble;
    }
    if (era_mia != (strcmp(h->aula, aula) == 0)) {
        hojas_guardar();                       /* entró o salió una hoja del aula */
    }
    cJSON_Delete(d);
}

/*
 * Muestra en la pantalla lo que mandó una hoja. Solo el ÚLTIMO valor: si llega uno más viejo
 * que el que ya se muestra (la cola de la hoja vaciando el historial para AURA), se reenvía
 * pero no se muestra.
 */
static void mostrar(hoja_t *h, const uint8_t mac[6], const aura_trama_t *t)
{
    if (h->aula[0] && strcmp(h->aula, aula) != 0) {
        return;                                        /* no es de esta aula: se reenvía pero no se muestra */
    }
    char hw[AURA_HW_ID_BYTES];
    aura_hw_id_de_mac(mac, hw);
    size_t cab = t->tipo == AURA_TIPO_TELEMETRIA ? AURA_TELEMETRIA_CABECERA : 4;
    if ((t->tipo == AURA_TIPO_TELEMETRIA || t->tipo == AURA_TIPO_ESTADO) && t->largo >= cab) {
        uint32_t ts = aura_u32_leer(t->payload + cab - 4);
        if (ts != 0 && h->ts_mostrado != 0 && ts < h->ts_mostrado) {
            return;                                    /* historial atrasado: ya se muestra algo más nuevo */
        }
        if (ts != 0) {
            h->ts_mostrado = ts;
        }
        char values[AURA_VALUES_MAX + 1];
        size_t n = t->largo - cab;
        memcpy(values, t->payload + cab, n);
        values[n] = '\0';
        printf("PANTALLA %s | %s | %s%s\n", aula, hw, values, t->tipo == AURA_TIPO_ESTADO ? " (estado)" : "");
        cJSON *v = cJSON_Parse(values);
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(v, "led");
        if (cJSON_IsNumber(l) && l->valueint != hoja_led) {
            hoja_led = l->valueint;
            led_mostrar(hoja_led);
            printf("PANTALLA %s | LED de la hoja: %s\n", aula, hoja_led ? "ENCENDIDO" : "apagado");
        }
        cJSON_Delete(v);
    } else if (t->tipo == AURA_TIPO_RESULTADO || t->tipo == AURA_TIPO_REPORTE || t->tipo == AURA_TIPO_ALERTA) {
        if (h->hay_evento && h->seq_evento == t->seq) {
            return;                                    /* reintento del mismo evento */
        }
        h->hay_evento = true;
        h->seq_evento = t->seq;
        char txt[AURA_PAYLOAD_MAX + 1];
        aura_payload_texto(t, txt, sizeof(txt));
        printf("PANTALLA %s | %s | %s %s\n", aula, hw,
               t->tipo == AURA_TIPO_RESULTADO ? "resultado" : t->tipo == AURA_TIPO_REPORTE ? "reporte" : "alerta", txt);
    }
}

/*
 * Lógica local del aula: decide si un dato de una hoja sigue hacia AURA.
 * Hoy deja pasar todo. Acá van, más adelante, las reglas que AURA le configure al
 * concentrador (filtrar, promediar, actuar localmente).
 * Ojo: una TELEMETRIA que no sigue nunca se confirma y la hoja la reintenta.
 */
static bool filtrar(const uint8_t mac[6], const aura_trama_t *t)
{
    return true;
}

/* ================================================================== */
/*  Recepción                                                          */
/* ================================================================== */
static void tarea_recepcion(void *arg)
{
    static uint8_t buf[MESH_MPS];
    while (true) {
        mesh_addr_t de;
        mesh_data_t d = {.data = buf, .size = sizeof(buf)};
        int flag = 0;
        if (esp_mesh_recv(&de, &d, portMAX_DELAY, &flag, NULL, 0) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (aura_trama_validar(buf, d.size) != AURA_TRAMA_OK) {
            ESP_LOGW(TAG, "Trama inválida de " MACSTR " (%u bytes)", MAC2STR(de.addr), d.size);
            continue;
        }
        aura_trama_t t;
        memcpy(&t, buf, d.size);

        /* Para este concentrador (de la raíz) */
        if (t.tipo == AURA_TIPO_COMANDO) {
            comando_t c = {.seq = t.seq};
            aura_payload_texto(&t, c.json, sizeof(c.json));
            xQueueSend(cola_comandos, &c, 0);
            continue;
        }
        if (t.tipo == AURA_TIPO_HORA && t.largo == 8) {
            hora_ajustar(aura_u64_leer(t.payload));
            continue;
        }
        if (t.tipo == AURA_TIPO_EVENTO_OK) {
            if (t.largo == 2 && hay_resultado_pendiente &&
                resultado_pendiente.seq == (uint16_t)(t.payload[0] | t.payload[1] << 8)) {
                hay_resultado_pendiente = false;
            }
            continue;
        }
        if (t.tipo == AURA_TIPO_REPORTAR) {
            reportar_ya = true;
            continue;
        }
        /* De una hoja del aula */
        hoja_t *h = hoja_vista(de.addr);
        aprender_aula(h, &t);
        if (t.tipo == AURA_TIPO_HORA) {                /* pedido de hora: si la tengo, contesto yo */
            uint64_t ms = ahora_ms();
            if (ms) {
                aura_trama_t r;
                xSemaphoreTake(lock_envio, portMAX_DELAY);
                aura_hora_armar(&r, seq++, ms);
                xSemaphoreGive(lock_envio);
                enviar_bytes(&de, (const uint8_t *)&r, aura_trama_bytes(&r));
                continue;
            }
        }
        mostrar(h, de.addr, &t);
        if (t.tipo == AURA_TIPO_ESTADO || isla) {
            continue;                                  /* ESTADO es solo para el aula; en isla no hay arriba */
        }
        if (filtrar(de.addr, &t)) {
            lote_agregar(de.addr, buf, d.size);
            if (t.tipo != AURA_TIPO_TELEMETRIA) {
                lote_mandar();                         /* resultados, reportes y alertas no esperan */
            }
        }
    }
}

/* ================================================================== */
/*  Aula, reporte y comandos propios                                   */
/* ================================================================== */
static void anunciar_aula(void)
{
    static uint8_t ie[sizeof(vendor_ie_data_t) + 6 + AURA_AULA_MAX];
    vendor_ie_data_t *v = (vendor_ie_data_t *)ie;
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_1, NULL);
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_1, NULL);
    if (aula[0] == '\0') {
        ESP_LOGW(TAG, "Sin aula: no se anuncia nada (la asigna AURA con set_config {\"aula\":...})");
        return;
    }
    v->element_id = WIFI_VENDOR_IE_ELEMENT_ID;
    v->vendor_oui[0] = AURA_OUI_0;
    v->vendor_oui[1] = AURA_OUI_1;
    v->vendor_oui[2] = AURA_OUI_2;
    v->vendor_oui_type = AURA_IE_CONCENTRADOR;
    memcpy(v->payload, mi_mac, 6);                     /* la dirección con la que se le habla en la malla */
    memcpy(v->payload + 6, aula, strlen(aula));
    v->length = 4 + 6 + strlen(aula);
    esp_err_t e1 = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_1, v);
    esp_err_t e2 = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_1, v);
    if (e1 == ESP_OK && e2 == ESP_OK) {
        ESP_LOGI(TAG, "Anunciando aula '%s' en el beacon", aula);
    } else {
        ESP_LOGW(TAG, "No se pudo anunciar el aula: %s / %s", esp_err_to_name(e1), esp_err_to_name(e2));
    }
}

static void describir(cJSON *config)
{
    if (aula[0]) {
        cJSON_AddStringToObject(config, "aula", aula);
    } else {
        cJSON_AddNullToObject(config, "aula");
    }
    cJSON_AddNumberToObject(config, "intervalo_s", INTERVALO_REPORTE_S);
}

static void reportar(void)
{
    cJSON *d = cJSON_CreateObject();
    describir(cJSON_AddObjectToObject(d, "config"));
    cJSON_AddStringToObject(d, "rol", "concentrador");
    cJSON_AddNumberToObject(d, "capa", esp_mesh_get_layer());
    cJSON_AddNumberToObject(d, "hijos", hijos);
    cJSON_AddNumberToObject(d, "hojas_aula", cant_hojas);
    cJSON_AddStringToObject(d, "alimentacion", "red");
    enviar_json(NULL, AURA_TIPO_REPORTE, d);
    cJSON_Delete(d);
}

static void atender_comando(const comando_t *c)
{
    static bool hay_ultimo = false;
    static uint16_t ultimo = 0;
    if (hay_ultimo && ultimo == c->seq) {
        return;                                        /* retransmisión */
    }
    hay_ultimo = true;
    ultimo = c->seq;
    ESP_LOGI(TAG, "Comando: %s", c->json);

    cJSON *d = cJSON_Parse(c->json);
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(d, "command");
    const cJSON *ci = cJSON_GetObjectItemCaseSensitive(d, "command_id");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(d, "params");
    bool ok = false;
    const char *motivo = "comando_desconocido";
    if (!cJSON_IsObject(d)) {
        motivo = "json_invalido";
    } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "set_config") == 0) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(params, "aula");
        if (cJSON_GetArraySize(params) != 1 || a == NULL) {
            motivo = "parametro_desconocido";          /* el concentrador solo tiene "aula" */
        } else if (!cJSON_IsString(a) || !aura_aula_valida(a->valuestring)) {
            motivo = "aula_invalida";
        } else {
            nvs_handle_t h;
            ok = nvs_open("concentrador", NVS_READWRITE, &h) == ESP_OK;
            if (ok) {
                ok = nvs_set_str(h, "aula", a->valuestring) == ESP_OK && nvs_commit(h) == ESP_OK;
                nvs_close(h);
            }
            if (ok) {
                bool cambio = strcmp(aula, a->valuestring) != 0;
                strlcpy(aula, a->valuestring, sizeof(aula));
                anunciar_aula();
                if (cambio) {
                    /* Las hojas colgadas son del aula anterior: se las suelta para que cada una
                       vuelva a elegir padre (las de esta aula vuelven, las otras van a plan B). */
                    ESP_LOGW(TAG, "Cambió el aula: suelto a las %d hojas conectadas", hijos);
                    esp_wifi_deauth_sta(0);
                    cant_hojas = 0;
                    hoja_led = -1;
                    hojas_guardar();                   /* lista vacía: las de la nueva aula se aprenden */
                    led_mostrar(0);
                }
            } else {
                motivo = "error_de_nvs";
            }
        }
    }
    cJSON *r = cJSON_CreateObject();
    if (cJSON_IsString(ci)) {
        cJSON_AddStringToObject(r, "command_id", ci->valuestring);
    }
    cJSON_AddBoolToObject(r, "aplicado", ok);
    if (!ok) {
        cJSON_AddStringToObject(r, "motivo", motivo);
    }
    describir(cJSON_AddObjectToObject(r, "config"));
    char *txt = cJSON_PrintUnformatted(r);
    if (txt && strlen(txt) <= AURA_PAYLOAD_MAX) {
        xSemaphoreTake(lock_envio, portMAX_DELAY);
        aura_trama_init(&resultado_pendiente, AURA_TIPO_RESULTADO, seq++, txt, strlen(txt));
        xSemaphoreGive(lock_envio);
        hay_resultado_pendiente = true;
        resultado_enviado_en = xTaskGetTickCount();
        enviar_bytes(NULL, (const uint8_t *)&resultado_pendiente, aura_trama_bytes(&resultado_pendiente));
    }
    cJSON_free(txt);
    cJSON_Delete(r);
    cJSON_Delete(d);
    if (ok) {
        reportar();
    }
}

static void canal_guardar(uint8_t canal)
{
    if (canal == 0 || canal == canal_conocido) {
        return;
    }
    canal_conocido = canal;
    nvs_handle_t h;
    if (nvs_open("concentrador", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "canal", canal);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void entrar_en_isla(void)
{
    ESP_LOGW(TAG, "%d s sin raíz: me reinicio como raíz de la isla del aula (canal %u)", ISLA_TRAS_S, canal_conocido);
    rtc_isla = RTC_ISLA;
    rtc_canal_isla = canal_conocido;
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void salir_de_isla(void)
{
    ESP_LOGW(TAG, "Volvió el gateway: me reinicio en modo normal");
    rtc_isla = 0;
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* En isla: ¿hay otra raíz de esta malla (el gateway) al alcance? */
static void isla_buscar_gateway(bool todos_los_canales)
{
    wifi_scan_config_t sc = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time.passive = 600,
        .channel = todos_los_canales ? 0 : rtc_canal_isla,
    };
    esp_wifi_scan_stop();
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        ESP_LOGW(TAG, "Isla: no se pudo escanear");
    }
}

static void isla_revisar_escaneo(int num)
{
    mesh_assoc_t assoc;
    wifi_ap_record_t rec;
    int ie_len = 0;
    bool gateway = false;
    for (int i = 0; i < num; i++) {
        esp_mesh_scan_get_ap_ie_len(&ie_len);
        esp_mesh_scan_get_ap_record(&rec, &assoc);
        if (ie_len == sizeof(assoc) && assoc.layer == 1 && memcmp(assoc.mesh_id, MESH_ID, 6) == 0) {
            ESP_LOGI(TAG, "Isla: veo una raíz de la malla en " MACSTR " (canal %d)", MAC2STR(rec.bssid), rec.primary);
            gateway = true;
        }
    }
    esp_mesh_flush_scan_result();
    if (gateway) {
        salir_de_isla();
    }
}

static void tarea_isla(void *arg)
{
    int vueltas = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(ISLA_REVISAR_S * 1000));
        bool todos = ++vueltas * ISLA_REVISAR_S >= ISLA_REVISAR_TODO_S;
        if (todos) {
            vueltas = 0;
        }
        isla_buscar_gateway(todos);
    }
}

static void tarea_estado(void *arg)
{
    TickType_t ultimo_reporte = 0, ultimo_pedido_hora = 0;
    comando_t c;
    while (true) {
        if (xQueueReceive(cola_comandos, &c, pdMS_TO_TICKS(200)) == pdTRUE && conectado) {
            atender_comando(&c);
        }
        static TickType_t ultima_revision = 0;
        if (xTaskGetTickCount() - ultima_revision >= pdMS_TO_TICKS(2000)) {
            ultima_revision = xTaskGetTickCount();
            revisar_ausentes();
        }
        if (!conectado) {
            /* Sin raíz: si dura, el aula pasa a modo isla (solo si ya se sabe en qué canal está) */
            TickType_t desde = sin_padre_desde;
            if (!isla && desde != 0 && canal_conocido != 0 &&
                xTaskGetTickCount() - desde >= pdMS_TO_TICKS(ISLA_TRAS_S * 1000)) {
                entrar_en_isla();
            }
            continue;
        }
        TickType_t ahora = xTaskGetTickCount();
        if (hay_resultado_pendiente && ahora - resultado_enviado_en >= pdMS_TO_TICKS(3000)) {
            resultado_enviado_en = ahora;             /* sin EVENTO_OK: se reintenta */
            enviar_bytes(NULL, (const uint8_t *)&resultado_pendiente, aura_trama_bytes(&resultado_pendiente));
        }
        if (reconecto || reportar_ya || ahora - ultimo_reporte >= pdMS_TO_TICKS(INTERVALO_REPORTE_S * 1000)) {
            reconecto = false;
            reportar_ya = false;
            ultimo_reporte = ahora;
            reportar();
        }
        if (ahora_ms() == 0 && (ultimo_pedido_hora == 0 || ahora - ultimo_pedido_hora >= pdMS_TO_TICKS(PEDIR_HORA_MS))) {
            ultimo_pedido_hora = ahora;
            enviar_trama(NULL, AURA_TIPO_HORA, NULL, 0);
        }
    }
}

/* Mando local: un toque en BOOT invierte el LED de la hoja */
static void tarea_boton(void *arg)
{
    int anterior = 1;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30));
        int nivel = gpio_get_level(PIN_BOTON);
        if (anterior == 1 && nivel == 0) {
            vTaskDelay(pdMS_TO_TICKS(30));             /* antirrebote */
            mesh_addr_t hoja;
            if (gpio_get_level(PIN_BOTON) == 0) {
                if (hoja_ultima(&hoja)) {                   /* no depende de la raíz: mando local */
                    const char *orden = "{\"command\":\"led\",\"params\":{\"invertir\":true},\"origen\":\"local\"}";
                    ESP_LOGI(TAG, "Botón: orden local a la hoja");
                    enviar_trama(&hoja, AURA_TIPO_COMANDO, orden, strlen(orden));
                } else {
                    ESP_LOGW(TAG, "Botón: todavía no hay hoja del aula");
                }
            }
        }
        anterior = nivel;
    }
}

static void manejador_mesh(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case MESH_EVENT_STARTED:
        if (isla) {
            esp_mesh_set_self_organized(false, false);   /* raíz de isla: no busca router */
        }
        anunciar_aula();
        break;
    case MESH_EVENT_SCAN_DONE:
        if (isla) {
            isla_revisar_escaneo(((mesh_event_scan_done_t *)data)->number);
        }
        break;
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *c = (mesh_event_connected_t *)data;
        conectado = true;
        sin_padre_desde = 0;
        canal_guardar(c->connected.channel);
        reconecto = true;
        fallos_envio = 0;
        ESP_LOGI(TAG, "Conectado al padre " MACSTR " - capa %d, canal %d", MAC2STR(c->connected.bssid),
                 esp_mesh_get_layer(), c->connected.channel);
        anunciar_aula();                               /* el AP puede haberse reiniciado al cambiar de canal */
        break;
    }
    case MESH_EVENT_PARENT_DISCONNECTED:
        if (conectado || sin_padre_desde == 0) {
            sin_padre_desde = xTaskGetTickCount() | 1;
        }
        conectado = false;
        ESP_LOGW(TAG, "Desconectado del padre, buscando...");
        break;
    case MESH_EVENT_NO_PARENT_FOUND:
        ESP_LOGW(TAG, "No se encontró padre (¿gateway encendido? ¿mismo MESH_ID?)");
        break;
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *c = (mesh_event_child_connected_t *)data;
        hijos++;
        for (int i = 0; i < cant_hojas; i++) {
            if (memcmp(hojas[i].mac, c->mac, 6) == 0) {
                hojas[i].desconectada_desde = 0;
            }
        }
        ESP_LOGI(TAG, "Hijo conectado: " MACSTR " (aid %d)", MAC2STR(c->mac), c->aid);
        break;
    }
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *c = (mesh_event_child_disconnected_t *)data;
        if (hijos > 0) {
            hijos--;
        }
        for (int i = 0; i < cant_hojas; i++) {
            if (memcmp(hojas[i].mac, c->mac, 6) == 0 && hojas[i].desconectada_desde == 0) {
                hojas[i].desconectada_desde = xTaskGetTickCount() | 1;
            }
        }
        ESP_LOGW(TAG, "Hijo desconectado: " MACSTR, MAC2STR(c->mac));
        break;
    }
    default:
        break;
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

    nvs_handle_t h;
    if (nvs_open("concentrador", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(aula);
        if (nvs_get_str(h, "aula", aula, &n) != ESP_OK || !aura_aula_valida(aula)) {
            aula[0] = '\0';
        }
        nvs_get_u8(h, "canal", &canal_conocido);
        nvs_close(h);
    }
    isla = rtc_isla == RTC_ISLA && rtc_canal_isla >= 1 && rtc_canal_isla <= 13;
    if (!isla) {
        rtc_isla = 0;
    }
    if (aula[0]) {
        hojas_cargar();
    }
    esp_read_mac(mi_mac, ESP_MAC_WIFI_STA);
    char hw[AURA_HW_ID_BYTES];
    aura_hw_id_de_mac(mi_mac, hw);
    ESP_LOGI(TAG, "Concentrador %s, aula %s%s", hw, aula[0] ? aula : "(sin asignar: la asigna AURA)",
             isla ? " — MODO ISLA (sin gateway)" : "");

    lock_envio = xSemaphoreCreateMutex();
    lock_lote = xSemaphoreCreateMutex();
    cola_comandos = xQueueCreate(4, sizeof(comando_t));
    aura_lote_vaciar(&lote);

    led_iniciar();
    gpio_config_t boton = {.pin_bit_mask = 1ULL << PIN_BOTON, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&boton);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif_sta = NULL, *netif_ap = NULL;
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, &netif_ap));
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_country_code("AR", false));   /* canales 1 a 13 */
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &manejador_mesh, NULL));
    if (isla) {
        ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT)); /* raíz de la isla del aula */
    }
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));          /* la raíz es fija: no compite */
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_CAPAS));
    ESP_ERROR_CHECK(esp_mesh_disable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK));

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&cfg.mesh_id, MESH_ID, 6);
    const char *router = isla ? ROUTER_ISLA : ROUTER_FICTICIO;
    cfg.channel = isla ? rtc_canal_isla : 0;           /* isla: el último canal; si no, buscar en todos */
    cfg.allow_channel_switch = !isla;
    cfg.router.ssid_len = strlen(router);
    memcpy(cfg.router.ssid, router, cfg.router.ssid_len);
    cfg.mesh_ap.max_connection = MESH_MAX_HIJOS;
    cfg.mesh_ap.nonmesh_max_connection = 0;
    memcpy(cfg.mesh_ap.password, MESH_AP_PASS, strlen(MESH_AP_PASS));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_send_block_time(LIMITE_ENVIO_MS));
    ESP_ERROR_CHECK(esp_mesh_start());

    xTaskCreate(tarea_recepcion, "mesh_rx", 6144, NULL, 5, NULL);
    xTaskCreate(tarea_lote, "lote", 3072, NULL, 5, NULL);
    xTaskCreate(tarea_estado, "estado", 4096, NULL, 4, NULL);
    xTaskCreate(tarea_boton, "boton", 3072, NULL, 4, NULL);
    if (isla) {
        xTaskCreate(tarea_isla, "isla", 3072, NULL, 3, NULL);
    } else {
        sin_padre_desde = xTaskGetTickCount() | 1;     /* hasta que se conecte a la raíz */
    }
}
