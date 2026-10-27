/*
 * Mesh de AURA - GATEWAY (ESP32-S3, raíz fija de la malla). Contrato MQTT v4.0.
 *
 * Es el único nodo con IP y el ADAPTADOR de la mesh en el sentido del contrato
 * (docs/CONTRATO_MQTT.md). Publica con el hw_id de cada placa ("mac-..."): no tiene tabla
 * de nodos que configurar ni conoce UUID. AURA traduce el hw_id a su dispositivo, o lo
 * pone en cuarentena (pendiente) si no lo conoce.
 *
 *   TELEMETRIA  -> hw/<hoja>/data (values, ingest_id, ts, adaptador)
 *   ack         <- hw/<hoja>/ack: con un resultado válido, CONFIRMACION a la hoja, que
 *                  recién ahí saca la muestra de su cola. El gateway no guarda muestras.
 *   REPORTE     -> hw/<nodo>/status, retenido, con los details del nodo (§3.2)
 *   RESULTADO   -> hw/<nodo>/response: recibido + aplicado / rechazado (§3.4)
 *   ALERTA      -> hw/<nodo>/alerts/<tipo> (§3.5)
 *                  Cada evento se confirma al nodo (EVENTO_OK) recién cuando se publicó: si el
 *                  broker no está, el nodo lo reintenta y no se pierde.
 *   Al volver el broker: REPORTAR a todos los nodos conocidos (reportan su estado actual y
 *                  mandan lo pendiente enseguida).
 *   comandos    <- hw/<nodo>/command, bajados por la mesh a la MAC del nodo (§3.3)
 *   HORA        -> la contesta a quien la pide; además va en cada CONFIRMACION
 *
 * Un LOTE (lo arma el concentrador del aula) trae tramas de varias hojas, cada una con la
 * MAC de la hoja: se publica cada una con el hw_id de su hoja.
 * Offline de cada nodo: si sale de la tabla de ruteo de la malla más de FUERA_DE_MALLA_MS
 * ("fuera_de_la_malla"), o si sigue en la malla pero pasa 3 x su intervalo sin hablar
 * ("sin_tramas"). Recuerda los nodos en NVS: tras reiniciarse, también marca offline a los que
 * no vuelven. Publica su propio status cada 60 s, con LWT.
 *
 * Actualización por la malla (ota_gateway.c): comando "ota" a cualquier placa de la mesh,
 * incluido este gateway (es el único comando que acepta para sí mismo). Tipo y versión de cada
 * placa: el "fw" de su reporte, publicado como details.firmware.
 *
 * Red y broker:
 * - Conoce una LISTA de redes (REDES_WIFI en credenciales.h): al arrancar usa la primera
 *   de la lista que esté al alcance. Si se queda sin router REINICIO_SIN_ROUTER_MS, se
 *   reinicia y empieza por la siguiente.
 * - Broker: si BROKER_URI está vacío, primero prueba la última dirección que le anduvo en
 *   esa red (NVS); si no conecta, lo busca por broadcast UDP "AURA_BROKER?" (broker.py
 *   contesta). Si lo pierde FALLOS_PARA_REBUSCAR veces seguidas, lo vuelve a buscar.
 *
 * MESH_ID y MESH_AP_PASS (config_local.h) deben ser IGUALES en todos los nodos.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
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
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "esp_crt_bundle.h"
#include "aura_trama.h"
#include "aura_util.h"
#include "aura_ota.h"
#include "aura_fw.h"
#include "gateway.h"
#include "ota_gateway.h"
#include "credenciales.h"

#if __has_include("config_local.h")
#include "config_local.h"
#else
#error "Falta main/config_local.h: copiá config_local.ejemplo.h como config_local.h y completá el ID y la clave de la mesh"
#endif
static const uint8_t MESH_ID[6] = {MESH_ID_BYTES};

/* Usuario y clave del broker (el de aura-app no acepta anónimos). Vacíos = anónimo. */
#ifndef MQTT_USUARIO
#define MQTT_USUARIO        ""
#endif
#ifndef MQTT_CLAVE
#define MQTT_CLAVE          ""
#endif
/* Descubrir el broker por broadcast: SOLO en el banco. Cualquiera en la misma red puede contestar,
   así que a un broker descubierto nunca se le mandan usuario y clave. En producción, BROKER_URI fija
   (y con mqtts://, el certificado del broker verificado). */
#ifndef DESCUBRIR_BROKER
#define DESCUBRIR_BROKER    0
#endif

#define MESH_MAX_HIJOS      6
#define MESH_MAX_CAPAS      6
#define CANAL_SIN_ROUTER    6                    /* si no se ve ninguna red de la lista */
#define SERVIDOR_NTP        "pool.ntp.org"
#define ZONA_HORARIA        "<-03>3"             /* Argentina (UTC-3), solo para los logs */
#define PUERTO_DESCUBRIMIENTO 18830              /* UDP; tiene que coincidir con broker.py */
#define FALLOS_PARA_REBUSCAR  3
#define REINICIO_SIN_ROUTER_MS (5 * 60 * 1000)   /* 0 = nunca */
#define NVS_ESPACIO         "gateway"
#define INTERVALO_STATUS_MS 60000
#define ESPERA_RESULTADO_MS 30000                /* comando sin resultado del nodo -> rechazado */
#define NODOS_CAP           64
#define EN_CURSO_CAP        8
#define DETAILS_MAX         320
#define FUERA_DE_MALLA_MS   20000                /* fuera de la tabla de ruteo tanto -> offline */
#define GRACIA_ARRANQUE_MS  60000                /* al arrancar, la malla tarda en armarse */
#define NVS_NODOS           "gw_nodos"
/* Consola: 1 = una línea por cada trama que llega de la malla (TRAMA <hw_id> <tipo> ...) y aviso de
   cada nodo nuevo. Sirve en el banco para ver las hojas de los grupos y lo que mandan. */
#define VER_TRAMAS          1

typedef struct {
    const char *ssid;
    const char *clave;
} red_wifi_t;

static const red_wifi_t REDES[] = { REDES_WIFI };
#define CANT_REDES          (sizeof(REDES) / sizeof(REDES[0]))

static const char *TAG = "GATEWAY";
static int red_actual = -1;
#define RTC_MAGICO          0x41555241u
static RTC_NOINIT_ATTR uint32_t rtc_magico;
static RTC_NOINIT_ATTR int rtc_red_fallida;
static volatile TickType_t sin_router_desde = 0;

static uint8_t rx_buf[1500];
static uint8_t mi_mac[6];
static char mi_hw[AURA_HW_ID_BYTES];
static esp_netif_t *netif_sta = NULL;
static esp_mqtt_client_handle_t mqtt = NULL;
static volatile bool mqtt_conectado = false;
static volatile bool tiene_ip = false;
static volatile bool buscar_broker = false;
static volatile bool probar_guardado = false;
static volatile int fallos_mqtt = 0;
static volatile bool pedir_reportes = false;     /* volvió el broker: lo atiende tarea_vigilancia */
static char uri_broker[64];
static char topico_lwt[AURA_TOPICO_MAX];

/* ---------- Lo que el gateway sabe de cada nodo que le habló ---------- */
typedef struct {
    bool usado;
    bool concentrador;                 /* se reportó con rol concentrador: puede mandar LOTE */
    uint8_t mac[6];
    uint32_t ultimo_ms;
    uint32_t intervalo_s;              /* del último reporte; 0 = no se infiere offline */
    bool offline;
    bool tiene_seq;
    uint16_t ultima_seq;               /* último evento publicado: un repetido se confirma sin publicar */
    uint32_t hora_dada_ms;             /* última vez que se le mandó la hora */
    uint32_t fuera_desde_ms;           /* 0 = está en la tabla de ruteo */
    char details[DETAILS_MAX];         /* del último reporte: para volver a online con ellos */
    char fw[2 * AURA_OTA_TEXTO];       /* "tipo@versión" del último reporte; "" = no lo informó */
} nodo_t;
static nodo_t nodos[NODOS_CAP];

typedef struct {
    bool activo;
    uint8_t mac[6];
    char command_id[48];
    uint32_t desde_ms;
} en_curso_t;
static en_curso_t en_curso[EN_CURSO_CAP];
static SemaphoreHandle_t lock = NULL;

/* Lo que llega por MQTT se atiende en otra tarea: esp_mesh_send puede tardar y no tiene
   que frenar al cliente MQTT. */
typedef struct {
    char topico[AURA_TOPICO_MAX];
    char datos[512];
    int largo;
} mensaje_mqtt_t;
static QueueHandle_t cola_mqtt = NULL;

static uint16_t seq = 0;
static volatile bool nodos_cambiaron = false;    /* hay que guardar la lista en NVS */
static uint32_t lotes_rechazados;
static uint32_t publicadas, confirmadas, acks_ignorados, ajenos, values_invalidos, repetidas,
    sin_lugar, version_distinta, invalidas, lotes;

/* ================================================================== */
/*  Hora                                                               */
/* ================================================================== */
static uint32_t ahora_ms32(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t ahora_epoca(void)
{
    time_t t = time(NULL);
    return (t > 0 && aura_hora_valida((uint32_t)t)) ? (uint32_t)t : 0;
}

static uint64_t ahora_utc_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ahora_epoca() ? (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 : 0;
}

static void hora_sincronizada(struct timeval *tv)
{
    char txt[32];
    struct tm t;
    localtime_r(&tv->tv_sec, &t);
    strftime(txt, sizeof(txt), "%d/%m/%Y %H:%M:%S", &t);
    ESP_LOGI(TAG, "Hora de internet: %s", txt);
}

static void iniciar_sntp(void)
{
    static bool iniciado = false;
    if (iniciado) {
        return;
    }
    iniciado = true;
    setenv("TZ", ZONA_HORARIA, 1);
    tzset();
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG(SERVIDOR_NTP);
    c.sync_cb = hora_sincronizada;
    esp_netif_sntp_init(&c);
}

/* ================================================================== */
/*  MQTT: publicar                                                     */
/* ================================================================== */
/* Sale enseguida (QoS 1). esp_mqtt_client_enqueue no bloquea, pero el cliente vacía su bandeja
   recién al volver de esperar datos del broker (hasta 1 s): cada mensaje se demoraba 0–1 s. */
static bool publicar(const char *topico, const char *cuerpo, bool retener)
{
    if (!mqtt_conectado) {
        ESP_LOGW(TAG, "Sin broker: no se publica %s", topico);
        return false;
    }
    if (esp_mqtt_client_publish(mqtt, topico, cuerpo, 0, 1, retener) >= 0) {
        publicadas++;
        ESP_LOGI(TAG, "MQTT -> %s %s", topico, cuerpo);
        return true;
    }
    ESP_LOGW(TAG, "No se pudo publicar en %s", topico);
    return false;
}

static bool publicar_json(const uint8_t mac[6], const char *sufijo, const cJSON *doc, bool retener)
{
    char topico[AURA_TOPICO_MAX];
    if (!aura_topico(topico, sizeof(topico), mac, sufijo)) {
        return false;
    }
    char *txt = cJSON_PrintUnformatted(doc);
    bool ok = false;
    if (txt) {
        ok = publicar(topico, txt, retener);
        cJSON_free(txt);
    }
    return ok;
}

/* response (§3.4). extra: campos que se agregan a details */
bool gw_responder(const uint8_t mac[6], const char *status, const char *command_id, const char *motivo,
                  const cJSON *extra)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "status", status);
    cJSON *det = cJSON_AddObjectToObject(d, "details");
    if (command_id && command_id[0]) {
        cJSON_AddStringToObject(det, "command_id", command_id);
    }
    if (motivo && motivo[0]) {
        cJSON_AddStringToObject(det, "motivo", motivo);
    }
    const cJSON *x;
    cJSON_ArrayForEach(x, extra) {
        cJSON_AddItemToObject(det, x->string, cJSON_Duplicate(x, true));
    }
    bool ok = publicar_json(mac, "response", d, false);
    cJSON_Delete(d);
    return ok;
}

static bool publicar_respuesta(const uint8_t mac[6], const char *status, const char *command_id,
                               const char *motivo, const cJSON *config)
{
    cJSON *x = NULL;
    if (config) {
        x = cJSON_CreateObject();
        cJSON_AddItemToObject(x, "config", cJSON_Duplicate(config, true));
    }
    bool ok = gw_responder(mac, status, command_id, motivo, x);
    cJSON_Delete(x);
    return ok;
}

bool gw_mqtt_conectado(void)
{
    return mqtt_conectado;
}

const uint8_t *gw_mi_mac(void)
{
    return mi_mac;
}

/* {"tipo":..., "version":...} a partir de "tipo@versión" */
static void agregar_firmware(cJSON *det, const char *fw)
{
    const char *arroba = strchr(fw, '@');
    if (arroba == NULL) {
        return;
    }
    char tipo[AURA_OTA_TEXTO];
    size_t n = (size_t)(arroba - fw) < sizeof(tipo) ? (size_t)(arroba - fw) : sizeof(tipo) - 1;
    memcpy(tipo, fw, n);
    tipo[n] = '\0';
    cJSON *f = cJSON_AddObjectToObject(det, "firmware");
    cJSON_AddStringToObject(f, "tipo", tipo);
    cJSON_AddStringToObject(f, "version", arroba + 1);
}

static void publicar_status_propio(void)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "status", "online");
    cJSON *det = cJSON_AddObjectToObject(d, "details");
    char m[18];
    snprintf(m, sizeof(m), MACSTR, MAC2STR(mi_mac));
    uint8_t canal = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&canal, &sec);
    cJSON_AddStringToObject(det, "transporte", "mesh");
    cJSON_AddStringToObject(det, "rol", "raiz");
    char fw[2 * AURA_OTA_TEXTO];
    aura_fw_texto(fw, sizeof(fw));
    agregar_firmware(det, fw);
    cJSON_AddStringToObject(det, "mac", m);
    cJSON_AddStringToObject(det, "red", red_actual >= 0 ? REDES[red_actual].ssid : "");
    cJSON_AddNumberToObject(det, "canal", canal);
    cJSON_AddNumberToObject(det, "nodos_en_mesh", esp_mesh_get_routing_table_size());
    cJSON_AddNumberToObject(det, "publicadas", publicadas);
    cJSON_AddNumberToObject(det, "confirmadas", confirmadas);
    cJSON_AddNumberToObject(det, "acks_ignorados", acks_ignorados);
    cJSON_AddNumberToObject(det, "values_invalidos", values_invalidos);
    cJSON_AddNumberToObject(det, "repetidas", repetidas);
    cJSON_AddNumberToObject(det, "ajenos", ajenos);
    cJSON_AddNumberToObject(det, "sin_lugar", sin_lugar);
    cJSON_AddNumberToObject(det, "version_distinta", version_distinta);
    cJSON_AddNumberToObject(det, "invalidas", invalidas);
    cJSON_AddNumberToObject(det, "lotes", lotes);
    cJSON_AddNumberToObject(det, "lotes_rechazados", lotes_rechazados);
    cJSON_AddBoolToObject(det, "hora_valida", ahora_epoca() != 0);
    cJSON_AddNumberToObject(det, "uptime_s", ahora_ms32() / 1000);
    ota_gw_estado(det);
    publicar_json(mi_mac, "status", d, true);
    cJSON_Delete(d);
}

void gw_publicar_status(void)
{
    if (mqtt_conectado) {
        publicar_status_propio();
    }
}

/* ================================================================== */
/*  Envío por la malla                                                 */
/* ================================================================== */
static bool enviar_a(const uint8_t mac[6], uint8_t tipo, const void *payload, size_t largo)
{
    aura_trama_t t;
    xSemaphoreTake(lock, portMAX_DELAY);
    aura_trama_init(&t, tipo, seq++, payload, largo);
    xSemaphoreGive(lock);
    mesh_addr_t destino;
    memcpy(destino.addr, mac, 6);
    mesh_data_t d = {.data = (uint8_t *)&t, .size = aura_trama_bytes(&t), .proto = MESH_PROTO_BIN, .tos = MESH_TOS_P2P};
    esp_err_t err = esp_mesh_send(&destino, &d, MESH_DATA_P2P, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo enviar a " MACSTR ": %s", MAC2STR(mac), esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static void dar_hora(const uint8_t mac[6])
{
    uint64_t ms = ahora_utc_ms();
    if (ms) {
        uint8_t p[8];
        aura_u64_escribir(p, ms);
        enviar_a(mac, AURA_TIPO_HORA, p, sizeof(p));
    }
}

/* ================================================================== */
/*  Nodos                                                              */
/* ================================================================== */
/* Con lock tomado */
static nodo_t *nodo(const uint8_t mac[6])
{
    nodo_t *libre = NULL;
    for (int i = 0; i < NODOS_CAP; i++) {
        if (nodos[i].usado && memcmp(nodos[i].mac, mac, 6) == 0) {
            return &nodos[i];
        }
        if (!nodos[i].usado && libre == NULL) {
            libre = &nodos[i];
        }
    }
    if (libre == NULL) {
        sin_lugar++;
        return NULL;
    }
    memset(libre, 0, sizeof(*libre));
    libre->usado = true;
    memcpy(libre->mac, mac, 6);
    libre->ultimo_ms = ahora_ms32();
    nodos_cambiaron = true;
#if VER_TRAMAS
    char hw[AURA_HW_ID_BYTES];
    aura_hw_id_de_mac(mac, hw);
    printf("NODO NUEVO %s\n", hw);
#endif
    return libre;
}

/* ===== Lista de nodos en NVS: se escribe solo si aparece uno nuevo o cambia un intervalo ===== */
typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    uint32_t intervalo_s;
} nodo_nvs_t;

static void nodos_guardar(void)
{
    nodo_nvs_t lista[NODOS_CAP];
    int n = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    nodos_cambiaron = false;
    for (int i = 0; i < NODOS_CAP; i++) {
        if (nodos[i].usado) {
            memcpy(lista[n].mac, nodos[i].mac, 6);
            lista[n].intervalo_s = nodos[i].intervalo_s;
            n++;
        }
    }
    xSemaphoreGive(lock);
    nvs_handle_t h;
    if (nvs_open(NVS_NODOS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "lista", lista, n * sizeof(nodo_nvs_t));
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Lista de nodos guardada (%d)", n);
    }
}

/* Al arrancar: los nodos que existían. Si no vuelven, se los marca offline. */
static void nodos_cargar(void)
{
    nodo_nvs_t lista[NODOS_CAP];
    size_t tam = sizeof(lista);
    nvs_handle_t h;
    if (nvs_open(NVS_NODOS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(h, "lista", lista, &tam) == ESP_OK) {
        int n = tam / sizeof(nodo_nvs_t);
        for (int i = 0; i < n && i < NODOS_CAP; i++) {
            nodos[i].usado = true;
            memcpy(nodos[i].mac, lista[i].mac, 6);
            nodos[i].intervalo_s = lista[i].intervalo_s;
            nodos[i].ultimo_ms = ahora_ms32();
        }
        ESP_LOGI(TAG, "Nodos conocidos de antes: %d", n);
    }
    nvs_close(h);
}

/* Volvió un nodo marcado offline: online con los últimos details conocidos. Si lo que llegó es un
   reporte, no: el reporte trae los details nuevos (por ejemplo, otra versión de firmware). */
static void marcar_visto(const uint8_t mac[6], bool con_details_viejos)
{
    char cuerpo[DETAILS_MAX + 48] = "";
    xSemaphoreTake(lock, portMAX_DELAY);
    nodo_t *n = nodo(mac);
    if (n) {
        n->ultimo_ms = ahora_ms32();
        n->fuera_desde_ms = 0;
        if (n->offline && con_details_viejos) {
            n->offline = false;
            snprintf(cuerpo, sizeof(cuerpo), "{\"status\":\"online\",\"details\":%s}", n->details[0] ? n->details : "{}");
        }
    }
    xSemaphoreGive(lock);
    if (cuerpo[0]) {
        char topico[AURA_TOPICO_MAX];
        aura_topico(topico, sizeof(topico), mac, "status");
        publicar(topico, cuerpo, true);
    }
}

/* true = ese evento (seq) ya se publicó: es un reintento del nodo */
static bool ya_publicado(const uint8_t mac[6], uint16_t s)
{
    bool rep = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    nodo_t *n = nodo(mac);
    if (n) {
        rep = n->tiene_seq && n->ultima_seq == s;
    }
    xSemaphoreGive(lock);
    return rep;
}

static void marcar_publicado(const uint8_t mac[6], uint16_t s)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    nodo_t *n = nodo(mac);
    if (n) {
        n->tiene_seq = true;
        n->ultima_seq = s;
    }
    xSemaphoreGive(lock);
}

/* ================================================================== */
/*  Subida: lo que manda un nodo                                       */
/* ================================================================== */
static void subir_telemetria(const uint8_t mac[6], const aura_trama_t *t)
{
    if (t->largo < AURA_TELEMETRIA_CABECERA) {
        values_invalidos++;
        return;
    }
    char id[37];
    aura_ingest_id_texto(t->payload, id);
    uint32_t ts = aura_u32_leer(t->payload + AURA_INGEST_ID_BYTES);
    char texto[AURA_VALUES_MAX + 1];
    size_t n = t->largo - AURA_TELEMETRIA_CABECERA;
    memcpy(texto, t->payload + AURA_TELEMETRIA_CABECERA, n);
    texto[n] = '\0';

    cJSON *d = cJSON_CreateObject();
    cJSON *values = cJSON_Parse(texto);
    /* values tiene que ser un objeto. Si no, se publica como texto: AURA lo rechaza con
       un ack y la hoja libera la muestra. Descartarlo acá la dejaría trabada en la hoja. */
    if (cJSON_IsObject(values)) {
        cJSON_AddItemToObject(d, "values", values);
    } else {
        cJSON_Delete(values);
        values_invalidos++;
        cJSON_AddStringToObject(d, "values", texto);
    }
    cJSON_AddStringToObject(d, "ingest_id", id);
    char iso[AURA_ISO_BYTES];
    if (aura_epoca_a_iso(ts, iso)) {
        cJSON_AddStringToObject(d, "ts", iso);         /* sin hora válida, sin ts */
    }
    cJSON_AddStringToObject(d, "adaptador", mi_hw);
    publicar_json(mac, "data", d, false);
    cJSON_Delete(d);

    /* La hoja todavía no tiene hora: se la da (como mucho una vez cada 30 s) */
    if (ts == 0) {
        bool dar = false;
        xSemaphoreTake(lock, portMAX_DELAY);
        nodo_t *nd = nodo(mac);
        if (nd && (nd->hora_dada_ms == 0 || ahora_ms32() - nd->hora_dada_ms > 30000)) {
            nd->hora_dada_ms = ahora_ms32() | 1;
            dar = true;
        }
        xSemaphoreGive(lock);
        if (dar) {
            dar_hora(mac);
        }
    }
}

static bool publicar_reporte(const uint8_t mac[6], cJSON *r)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "status", "online");
    cJSON *det = cJSON_Duplicate(r, true);
    /* "fw":"tipo@versión" (corto, por los 180 B de la trama) -> "firmware":{"tipo","version"} */
    char fw[2 * AURA_OTA_TEXTO] = "";
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(r, "fw");
    if (cJSON_IsString(f) && strlen(f->valuestring) < sizeof(fw)) {
        strcpy(fw, f->valuestring);
        agregar_firmware(det, fw);
    }
    cJSON_DeleteItemFromObject(det, "fw");
    cJSON_AddStringToObject(det, "evento", "reporte");
    cJSON_AddStringToObject(det, "transporte", "mesh");
    if (!cJSON_HasObjectItem(det, "alimentacion")) {
        cJSON_AddStringToObject(det, "alimentacion", "desconocida");
    }
    cJSON_AddItemToObject(d, "details", det);

    const cJSON *iv = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "config"), "intervalo_s");
    const cJSON *rol = cJSON_GetObjectItemCaseSensitive(r, "rol");
    char *txt = cJSON_PrintUnformatted(det);
    xSemaphoreTake(lock, portMAX_DELAY);
    nodo_t *n = nodo(mac);
    if (n) {
        uint32_t iv_nuevo = cJSON_IsNumber(iv) ? (uint32_t)iv->valuedouble : 0;
        if (iv_nuevo != n->intervalo_s) {
            n->intervalo_s = iv_nuevo;
            nodos_cambiaron = true;
        }
        n->offline = false;
        n->concentrador = cJSON_IsString(rol) && strcmp(rol->valuestring, "concentrador") == 0;
        if (txt && strlen(txt) < sizeof(n->details)) {
            strcpy(n->details, txt);
        }
        if (fw[0]) {
            strcpy(n->fw, fw);
        }
    }
    xSemaphoreGive(lock);
    cJSON_free(txt);
    bool ok = publicar_json(mac, "status", d, true);
    cJSON_Delete(d);
    if (fw[0]) {
        ota_gw_reporte(mac, fw);                       /* ¿volvió de una actualización? (con broker o sin) */
    }
    return ok;
}

bool gw_fw_de_nodo(const uint8_t mac[6], char *fw, size_t tam)
{
    bool hay = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < NODOS_CAP && !hay; i++) {
        if (nodos[i].usado && memcmp(nodos[i].mac, mac, 6) == 0 && nodos[i].fw[0]) {
            strlcpy(fw, nodos[i].fw, tam);
            hay = true;
        }
    }
    xSemaphoreGive(lock);
    return hay;
}

static void quitar_en_curso(const uint8_t mac[6], const char *command_id)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < EN_CURSO_CAP; i++) {
        if (en_curso[i].activo && memcmp(en_curso[i].mac, mac, 6) == 0 &&
            strcmp(en_curso[i].command_id, command_id ? command_id : "") == 0) {
            en_curso[i].activo = false;
        }
    }
    xSemaphoreGive(lock);
}

static bool publicar_resultado(const uint8_t mac[6], const cJSON *r)
{
    const cJSON *ci = cJSON_GetObjectItemCaseSensitive(r, "command_id");
    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(r, "aplicado");
    const cJSON *mo = cJSON_GetObjectItemCaseSensitive(r, "motivo");
    const char *command_id = cJSON_IsString(ci) ? ci->valuestring : NULL;
    quitar_en_curso(mac, command_id);
    bool ok = publicar_respuesta(mac, "recibido", command_id, NULL, NULL);
    ok = publicar_respuesta(mac, cJSON_IsTrue(ap) ? "aplicado" : "rechazado", command_id,
                            cJSON_IsString(mo) ? mo->valuestring : (cJSON_IsTrue(ap) ? "" : "rechazado_por_el_nodo"),
                            cJSON_GetObjectItemCaseSensitive(r, "config")) && ok;
    return ok;
}

static bool publicar_alerta(const uint8_t mac[6], const cJSON *r)
{
    const cJSON *tipo = cJSON_GetObjectItemCaseSensitive(r, "tipo");
    if (!cJSON_IsString(tipo) || !aura_alerta_tipo_valido(tipo->valuestring)) {
        ESP_LOGW(TAG, "Alerta con tipo inválido: no se publica");
        return true;                                   /* no tiene arreglo: que no se reintente */
    }
    const cJSON *sev = cJSON_GetObjectItemCaseSensitive(r, "severity");
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(r, "message");
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "severity", cJSON_IsString(sev) ? sev->valuestring : "unknown");
    cJSON_AddStringToObject(d, "message", cJSON_IsString(msg) ? msg->valuestring : "");
    const cJSON *det = cJSON_GetObjectItemCaseSensitive(r, "details");
    if (det) {
        cJSON_AddItemToObject(d, "details", cJSON_Duplicate(det, true));
    }
    char iso[AURA_ISO_BYTES];
    if (aura_epoca_a_iso(ahora_epoca(), iso)) {
        cJSON_AddStringToObject(d, "ts", iso);
    }
    char sufijo[40];
    snprintf(sufijo, sizeof(sufijo), "alerts/%s", tipo->valuestring);
    bool ok = publicar_json(mac, sufijo, d, false);
    cJSON_Delete(d);
    return ok;
}

/* El evento con esa seq ya está publicado: el nodo lo saca de su lista */
static void confirmar_evento(const uint8_t mac[6], uint16_t s)
{
    uint8_t p[2] = {(uint8_t)s, (uint8_t)(s >> 8)};
    enviar_a(mac, AURA_TIPO_EVENTO_OK, p, sizeof(p));
}

/* Una trama de un nodo (sola o sacada de un LOTE) */
static void procesar(const uint8_t mac[6], const uint8_t *datos, size_t largo)
{
    aura_validacion_t v = aura_trama_validar(datos, largo);
    if (v == AURA_TRAMA_VERSION) {
        version_distinta++;
#if VER_TRAMAS
        printf("TRAMA " MACSTR " de otra versión de la trama (%u bytes): se descarta\n", MAC2STR(mac), (unsigned)largo);
#endif
        return;
    }
    if (v != AURA_TRAMA_OK) {
        invalidas++;
#if VER_TRAMAS
        printf("TRAMA " MACSTR " inválida (%u bytes): se descarta\n", MAC2STR(mac), (unsigned)largo);
#endif
        return;
    }
    aura_trama_t t;
    memcpy(&t, datos, largo);
#if VER_TRAMAS
    char hw[AURA_HW_ID_BYTES], txt[AURA_PAYLOAD_MAX + 1];
    aura_hw_id_de_mac(mac, hw);
    aura_trama_describir(&t, txt, sizeof(txt));
    printf("TRAMA %s %s seq=%u %s\n", hw, aura_tipo_nombre(t.tipo), t.seq, txt);
#endif
    if (t.tipo == AURA_TIPO_HORA) {                    /* pedido de hora */
        dar_hora(mac);
        return;
    }
    if (t.tipo != AURA_TIPO_TELEMETRIA && t.tipo != AURA_TIPO_REPORTE &&
        t.tipo != AURA_TIPO_RESULTADO && t.tipo != AURA_TIPO_ALERTA) {
        return;
    }
    marcar_visto(mac, t.tipo != AURA_TIPO_REPORTE);
    if (t.tipo == AURA_TIPO_TELEMETRIA) {              /* cada reintento se publica: AURA deduplica */
        subir_telemetria(mac, &t);
        return;
    }
    /* Evento: se confirma solo si quedó publicado (o ya lo estaba: reintento del nodo) */
    if (ya_publicado(mac, t.seq)) {
        repetidas++;
        confirmar_evento(mac, t.seq);
        return;
    }
    char json[AURA_PAYLOAD_MAX + 1];
    aura_payload_texto(&t, json, sizeof(json));
    cJSON *d = cJSON_Parse(json);
    bool ok = true;                                    /* uno mal armado no tiene arreglo: se confirma */
    if (!cJSON_IsObject(d)) {
        values_invalidos++;
    } else if (t.tipo == AURA_TIPO_REPORTE) {
        ok = publicar_reporte(mac, d);
    } else if (t.tipo == AURA_TIPO_RESULTADO) {
        ok = publicar_resultado(mac, d);
    } else {
        ok = publicar_alerta(mac, d);
    }
    cJSON_Delete(d);
    if (ok) {
        marcar_publicado(mac, t.seq);
        confirmar_evento(mac, t.seq);
    }
}

static bool es_concentrador(const uint8_t mac[6])
{
    bool es = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < NODOS_CAP && !es; i++) {
        es = nodos[i].usado && nodos[i].concentrador && memcmp(nodos[i].mac, mac, 6) == 0;
    }
    xSemaphoreGive(lock);
    return es;
}

static void tarea_recepcion(void *arg)
{
    while (true) {
        mesh_addr_t origen;
        mesh_data_t data = {.data = rx_buf, .size = sizeof(rx_buf)};
        int flag = 0;
        if (esp_mesh_recv(&origen, &data, portMAX_DELAY, &flag, NULL, 0) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (aura_es_ota(rx_buf, data.size)) {         /* un nodo que se está actualizando */
            ota_gw_trama(origen.addr, rx_buf, data.size);
        } else if (aura_es_lote(rx_buf, data.size) && !es_concentrador(origen.addr)) {
            /* Un LOTE trae datos a nombre de otras placas: solo se acepta de un concentrador (un nodo
               que se reportó con ese rol). De otro nodo se descarta. */
            lotes_rechazados++;
            ESP_LOGW(TAG, "LOTE de " MACSTR ", que no es un concentrador: se descarta", MAC2STR(origen.addr));
        } else if (aura_es_lote(rx_buf, data.size)) {
            /* Del concentrador: cada trama con la MAC de la hoja que la generó */
            lotes++;
            size_t pos = 0, largo;
            const uint8_t *mac, *trama;
            while (aura_lote_siguiente(rx_buf, data.size, &pos, &mac, &trama, &largo)) {
                uint8_t m[6];
                memcpy(m, mac, 6);
                procesar(m, trama, largo);
            }
        } else {
            procesar(origen.addr, rx_buf, data.size);
        }
    }
}

/* ================================================================== */
/*  Bajada: lo que manda AURA                                          */
/* ================================================================== */
static void procesar_ack(const uint8_t mac[6], const char *msg)
{
    cJSON *d = cJSON_Parse(msg);
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(d, "resultado");
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(d, "ingest_id");
    uint8_t id[AURA_INGEST_ID_BYTES];
    /* Ante cualquier duda no se confirma: la hoja reintenta y AURA dice "duplicado".
       Confirmar de más borraría una muestra. */
    if (!cJSON_IsString(r) || !aura_ack_confirma(r->valuestring) || !cJSON_IsString(i) ||
        !aura_ingest_id_de_texto(i->valuestring, id)) {
        acks_ignorados++;
        ESP_LOGW(TAG, "ack ignorado: %s", msg);
        cJSON_Delete(d);
        return;
    }
    cJSON_Delete(d);
    uint8_t p[AURA_CONFIRMACION_BYTES];
    memcpy(p, id, AURA_INGEST_ID_BYTES);
    aura_u32_escribir(p + AURA_INGEST_ID_BYTES, ahora_epoca());
    if (enviar_a(mac, AURA_TIPO_CONFIRMACION, p, sizeof(p))) {
        confirmadas++;
    } else {
        ESP_LOGW(TAG, "La hoja no está alcanzable: reintentará y AURA dirá duplicado");
    }
}

static void procesar_comando(const uint8_t mac[6], const char *msg, int largo)
{
    cJSON *d = cJSON_Parse(msg);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(d, "command");
    const cJSON *ci = cJSON_GetObjectItemCaseSensitive(d, "command_id");
    char command_id[48] = "";
    if (cJSON_IsString(ci)) {
        strlcpy(command_id, ci->valuestring, sizeof(command_id));
    }
    bool valido = cJSON_IsObject(d) && cJSON_IsString(c);
    bool ota = valido && strcmp(c->valuestring, "ota") == 0;
    cJSON_Delete(d);
    if (!valido) {
        publicar_respuesta(mac, "rechazado", command_id, "json_invalido", NULL);
        return;
    }
    if (ota) {                                         /* lo atiende el gateway (también para sí mismo) */
        ota_gw_pedido(mac, msg);
        return;
    }
    if (memcmp(mac, mi_mac, 6) == 0) {
        publicar_respuesta(mac, "rechazado", command_id, "el_raiz_no_acepta_comandos", NULL);
        return;
    }
    if (largo > AURA_PAYLOAD_MAX) {                    /* no se trunca: llegaría otro comando */
        publicar_respuesta(mac, "rechazado", command_id, "comando_muy_grande", NULL);
        return;
    }
    /* Se anota ANTES de enviar: un nodo cercano contesta el resultado antes de que
       esp_mesh_send termine, y si no estaba anotado vencería como sin_resultado_del_nodo. */
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < EN_CURSO_CAP; i++) {
        if (!en_curso[i].activo) {
            en_curso[i].activo = true;
            memcpy(en_curso[i].mac, mac, 6);
            strlcpy(en_curso[i].command_id, command_id, sizeof(en_curso[i].command_id));
            en_curso[i].desde_ms = ahora_ms32();
            break;
        }
    }
    xSemaphoreGive(lock);
    if (!enviar_a(mac, AURA_TIPO_COMANDO, msg, largo)) {
        quitar_en_curso(mac, command_id);
        publicar_respuesta(mac, "rechazado", command_id, "nodo_no_alcanzable", NULL);
        return;
    }
    publicar_respuesta(mac, "transmitido", command_id, NULL, NULL);
}

static void tarea_bajada(void *arg)
{
    static mensaje_mqtt_t m;
    while (true) {
        if (xQueueReceive(cola_mqtt, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint8_t mac[6];
        char tipo[8];
        if (!aura_parsear_topico(m.topico, strlen(m.topico), mac, tipo)) {
            ajenos++;                                  /* LoRaWAN u otro adaptador */
            continue;
        }
        ESP_LOGI(TAG, "MQTT <- %s %s", m.topico, m.datos);
        if (strcmp(tipo, "ack") == 0) {
            procesar_ack(mac, m.datos);
        } else {
            procesar_comando(mac, m.datos, m.largo);
        }
    }
}

/* ================================================================== */
/*  Vigilancia: comandos vencidos, offline inferido, status propio     */
/* ================================================================== */
/* Volvió el broker: cada nodo conocido reporta su estado actual y manda lo pendiente */
static void pedir_reporte_a_todos(void)
{
    uint8_t macs[NODOS_CAP][6];
    int n = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < NODOS_CAP; i++) {
        if (nodos[i].usado && !nodos[i].offline) {
            memcpy(macs[n++], nodos[i].mac, 6);
        }
    }
    xSemaphoreGive(lock);
    if (n) {
        ESP_LOGI(TAG, "Volvió el broker: pido reporte a %d nodos", n);
    }
    for (int i = 0; i < n; i++) {
        enviar_a(macs[i], AURA_TIPO_REPORTAR, NULL, 0);
    }
}

static void tarea_vigilancia(void *arg)
{
    uint32_t ultimo_status = 0;
    int vueltas = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (pedir_reportes && mqtt_conectado) {
            pedir_reportes = false;
            pedir_reporte_a_todos();
        }
        if (++vueltas < 5) {
            continue;
        }
        vueltas = 0;
        uint32_t ahora = ahora_ms32();
        en_curso_t vencidos[EN_CURSO_CAP];
        int nv = 0;
        uint8_t offline[NODOS_CAP][6];
        const char *motivo[NODOS_CAP];
        int no = 0;
        /* Tabla de ruteo de la malla: quién está colgado de este gateway ahora */
        static mesh_addr_t ruteo[NODOS_CAP + 8];
        int en_tabla = 0;
        esp_mesh_get_routing_table(ruteo, sizeof(ruteo), &en_tabla);
        bool revisar_ruteo = ahora >= GRACIA_ARRANQUE_MS;
        xSemaphoreTake(lock, portMAX_DELAY);
        for (int i = 0; i < EN_CURSO_CAP; i++) {
            if (en_curso[i].activo && ahora - en_curso[i].desde_ms >= ESPERA_RESULTADO_MS) {
                en_curso[i].activo = false;
                vencidos[nv++] = en_curso[i];
            }
        }
        for (int i = 0; i < NODOS_CAP; i++) {
            nodo_t *n = &nodos[i];
            if (!n->usado || n->offline) {
                continue;
            }
            bool esta = false;
            for (int k = 0; k < en_tabla && !esta; k++) {
                esta = memcmp(ruteo[k].addr, n->mac, 6) == 0;
            }
            if (esta) {
                n->fuera_desde_ms = 0;
            } else if (n->fuera_desde_ms == 0) {
                n->fuera_desde_ms = ahora | 1;
            }
            if (revisar_ruteo && !esta && ahora - n->fuera_desde_ms >= FUERA_DE_MALLA_MS) {
                motivo[no] = "fuera_de_la_malla";
                memcpy(offline[no++], n->mac, 6);
            } else if (revisar_ruteo && aura_debe_marcar_offline(ahora, n->ultimo_ms, n->intervalo_s, false)) {
                /* También con margen al arrancar: los nodos recordados en NVS tardan en volver a colgarse
                   (con el gateway recién encendido se publicaba un offline falso por 1 s, 2026-10-22) */
                motivo[no] = "sin_tramas";
                memcpy(offline[no++], n->mac, 6);
            }
        }
        xSemaphoreGive(lock);
        for (int i = 0; i < nv; i++) {
            publicar_respuesta(vencidos[i].mac, "rechazado", vencidos[i].command_id, "sin_resultado_del_nodo", NULL);
        }
        for (int i = 0; i < no; i++) {
            char topico[AURA_TOPICO_MAX], cuerpo[96];
            aura_topico(topico, sizeof(topico), offline[i], "status");
            snprintf(cuerpo, sizeof(cuerpo), "{\"status\":\"offline\",\"details\":{\"motivo\":\"%s\"}}", motivo[i]);
            if (publicar(topico, cuerpo, true)) {        /* sin broker se vuelve a intentar */
                xSemaphoreTake(lock, portMAX_DELAY);
                nodo_t *n = nodo(offline[i]);
                if (n) {
                    n->offline = true;
                }
                xSemaphoreGive(lock);
            }
        }
        if (nodos_cambiaron) {
            nodos_guardar();
        }
        if (mqtt_conectado && (ultimo_status == 0 || ahora - ultimo_status >= INTERVALO_STATUS_MS)) {
            ultimo_status = ahora | 1;
            publicar_status_propio();
        }
    }
}

/* ================================================================== */
/*  Broker                                                             */
/* ================================================================== */
static void clave_broker(char *clave, size_t tam)
{
    snprintf(clave, tam, "broker%d", red_actual);
}

static bool broker_guardado_leer(char *uri, size_t tam)
{
    if (red_actual < 0) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_ESPACIO, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char clave[16];
    clave_broker(clave, sizeof(clave));
    size_t largo = tam;
    bool ok = nvs_get_str(h, clave, uri, &largo) == ESP_OK && uri[0] != '\0';
    nvs_close(h);
    return ok;
}

/* Se llama al conectar: solo escribe si la dirección cambió (cuida la flash) */
static void broker_guardado_escribir(const char *uri)
{
    char previo[64];
    if (red_actual < 0 || (broker_guardado_leer(previo, sizeof(previo)) && strcmp(previo, uri) == 0)) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_ESPACIO, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char clave[16];
    clave_broker(clave, sizeof(clave));
    if (nvs_set_str(h, clave, uri) == ESP_OK && nvs_commit(h) == ESP_OK) {
        ESP_LOGI(TAG, "Broker guardado para la red %s: %s", REDES[red_actual].ssid, uri);
    }
    nvs_close(h);
}

static void manejador_mqtt(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_conectado = true;
        fallos_mqtt = 0;
        buscar_broker = false;
        ESP_LOGI(TAG, "MQTT conectado como %s", mi_hw);
        aura_fw_confirmar();                           /* llega al broker: la versión anda */
        if (BROKER_URI[0] == '\0') {
            broker_guardado_escribir(uri_broker);
        }
        esp_mqtt_client_subscribe(mqtt, "hw/+/ack", 1);
        esp_mqtt_client_subscribe(mqtt, "hw/+/command", 1);
        publicar_status_propio();
        ota_gw_broker_conectado();                     /* si se acaba de actualizar a sí mismo */
        pedir_reportes = true;                         /* que los nodos actualicen su estado */
        break;
    case MQTT_EVENT_DATA:
        /* Mensajes chicos: llegan completos en un solo evento */
        if (ev->data_len == ev->total_data_len && ev->topic_len < AURA_TOPICO_MAX &&
            ev->data_len < (int)sizeof(((mensaje_mqtt_t *)0)->datos)) {
            mensaje_mqtt_t m;
            memcpy(m.topico, ev->topic, ev->topic_len);
            m.topico[ev->topic_len] = '\0';
            memcpy(m.datos, ev->data, ev->data_len);
            m.datos[ev->data_len] = '\0';
            m.largo = ev->data_len;
            if (xQueueSend(cola_mqtt, &m, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Cola de bajada llena: se descarta %s", m.topico);
            }
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_conectado = false;
        fallos_mqtt++;
        if (BROKER_URI[0] == '\0' && fallos_mqtt >= FALLOS_PARA_REBUSCAR) {
            ESP_LOGW(TAG, "MQTT: %d intentos sin broker, lo vuelvo a buscar", fallos_mqtt);
            buscar_broker = true;
        } else {
            ESP_LOGW(TAG, "MQTT desconectado (reintenta solo)");
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

/* Pregunta por broadcast "AURA_BROKER?" y espera "AURA_BROKER <puerto>" de broker.py (~2 s) */
static bool descubrir_broker(char *uri, size_t tam)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return false;
    }
    int si = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &si, sizeof(si));
    struct timeval espera = {.tv_sec = 2};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &espera, sizeof(espera));
    const char *pregunta = "AURA_BROKER?";
    struct sockaddr_in destino = {
        .sin_family = AF_INET,
        .sin_port = htons(PUERTO_DESCUBRIMIENTO),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(s, pregunta, strlen(pregunta), 0, (struct sockaddr *)&destino, sizeof(destino));
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif_sta, &ip) == ESP_OK) {      /* también al broadcast de la subred */
        destino.sin_addr.s_addr = ip.ip.addr | ~ip.netmask.addr;
        sendto(s, pregunta, strlen(pregunta), 0, (struct sockaddr *)&destino, sizeof(destino));
    }
    char resp[48];
    struct sockaddr_in origen;
    socklen_t largo = sizeof(origen);
    int n = recvfrom(s, resp, sizeof(resp) - 1, 0, (struct sockaddr *)&origen, &largo);
    close(s);
    int puerto;
    if (n <= 0) {
        return false;
    }
    resp[n] = '\0';
    if (sscanf(resp, "AURA_BROKER %d", &puerto) != 1) {
        return false;
    }
    snprintf(uri, tam, "mqtt://%s:%d", inet_ntoa(origen.sin_addr), puerto);
    return true;
}

/*
 * Busca el broker cuando hace falta y (re)arranca el cliente MQTT.
 * Orden: BROKER_URI fija; si no hay, el último broker que anduvo en esta red (una vez por
 * conexión al router); si ese falla FALLOS_PARA_REBUSCAR veces, el broadcast.
 */
static void tarea_broker(void *arg)
{
    char *uri = uri_broker;
    int intentos = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!tiene_ip || !buscar_broker) {
            continue;
        }
        if (BROKER_URI[0] != '\0') {
            strlcpy(uri, BROKER_URI, sizeof(uri_broker));
        } else if (!DESCUBRIR_BROKER) {
            if (++intentos % 60 == 1) {
                ESP_LOGE(TAG, "Sin BROKER_URI en credenciales.h (el descubrimiento es solo para el banco)");
            }
            continue;
        } else if (probar_guardado && broker_guardado_leer(uri, sizeof(uri_broker))) {
            probar_guardado = false;
            ESP_LOGI(TAG, "Pruebo el último broker que anduvo en esta red (si no responde, lo busco)");
        } else if (!descubrir_broker(uri, sizeof(uri_broker))) {
            if (++intentos % 5 == 1) {
                ESP_LOGW(TAG, "No encuentro el broker (¿broker.py corriendo? ¿misma red? ¿firewall?)");
            }
            continue;
        }
        intentos = 0;
        buscar_broker = false;
        fallos_mqtt = 0;
        ESP_LOGI(TAG, "Broker: %s", uri);
        if (mqtt == NULL) {
            /* client_id fijo = su hw_id y sesión persistente (contrato §5): mientras reconecta,
               el broker le guarda los ack y command QoS 1. LWT: offline retenido. */
            esp_mqtt_client_config_t c = {
                .broker.address.uri = uri,
                .credentials.client_id = mi_hw,
                /* Usuario y clave solo a la BROKER_URI fija: nunca a un broker descubierto */
                .credentials.username = (BROKER_URI[0] && MQTT_USUARIO[0]) ? MQTT_USUARIO : NULL,
                .credentials.authentication.password = (BROKER_URI[0] && MQTT_CLAVE[0]) ? MQTT_CLAVE : NULL,
                /* mqtts://: se verifica el certificado del broker (CA propia o las públicas) */
#ifdef BROKER_CA_PEM
                .broker.verification.certificate = BROKER_CA_PEM,
#else
                .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
#endif
                .session.disable_clean_session = true,
                .session.keepalive = 30,           /* el broker nota la caída en ~45 s y publica el LWT */
                .session.last_will.topic = topico_lwt,
                .session.last_will.msg = "{\"status\":\"offline\"}",
                .session.last_will.qos = 1,
                .session.last_will.retain = 1,
                .buffer.size = 2048,
            };
            mqtt = esp_mqtt_client_init(&c);
            esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, manejador_mqtt, NULL);
            esp_mqtt_client_start(mqtt);
        } else {
            esp_mqtt_client_stop(mqtt);
            esp_mqtt_client_set_uri(mqtt, uri);
            esp_mqtt_client_start(mqtt);
        }
    }
}

/* ================================================================== */
/*  Malla y red                                                        */
/* ================================================================== */
static void manejador_eventos_mesh(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    switch (id) {
    case MESH_EVENT_STARTED: {
        /* Sin PMF (802.11w) en el AP de la malla. Con PMF, cuando el padre se reinicia el hijo tarda
           ~17 s en enterarse (SA query) y la radio muestra "MIC mismatch" en los beacons; sin PMF, el
           AP nuevo lo desconecta en el acto. Tiene que ir acá: antes de esp_mesh_start() la malla lo
           vuelve a activar (probado 2026-10-22). */
        esp_wifi_disable_pmf_config(WIFI_IF_AP);
        ESP_LOGI(TAG, "Malla iniciada. Raíz: %s, capa %d", esp_mesh_is_root() ? "sí" : "no", esp_mesh_get_layer());
        /* "Soy el gateway" en el beacon: así un concentrador en isla no confunde con el gateway a otro
           concentrador en isla (también raíz de capa 1 con el mismo MESH_ID). No depende de cómo se
           conecta el gateway a la red (WiFi o Ethernet): va en el beacon de la malla. */
        static vendor_ie_data_t ie = {
            .element_id = WIFI_VENDOR_IE_ELEMENT_ID,
            .length = 4,
            .vendor_oui = {AURA_OUI_0, AURA_OUI_1, AURA_OUI_2},
            .vendor_oui_type = AURA_IE_GATEWAY,
        };
        esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_1, &ie);
        esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_1, &ie);
        break;
    }
    case MESH_EVENT_PARENT_CONNECTED: {
        /* En la raíz, el "padre" es el router: se pide IP por DHCP */
        mesh_event_connected_t *c = (mesh_event_connected_t *)event_data;
        ESP_LOGI(TAG, "Conectado al router " MACSTR " (canal %d)", MAC2STR(c->connected.bssid), c->connected.channel);
        if (esp_mesh_is_root()) {
            esp_netif_dhcpc_stop(netif_sta);
            esp_netif_dhcpc_start(netif_sta);
        }
        break;
    }
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *d = (mesh_event_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Sin conexión al router (motivo %d), reintentando...", d->reason);
        if (sin_router_desde == 0) {
            sin_router_desde = xTaskGetTickCount() | 1;   /* | 1: nunca 0, que es "conectado" */
        }
        break;
    }
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *c = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(TAG, "Hijo conectado: " MACSTR " (aid %d)", MAC2STR(c->mac), c->aid);
        break;
    }
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *c = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Hijo desconectado: " MACSTR, MAC2STR(c->mac));
        break;
    }
    case MESH_EVENT_ROUTING_TABLE_ADD:
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *r = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGI(TAG, "Tabla de ruteo %s %d -> total en la malla: %d nodos",
                 id == MESH_EVENT_ROUTING_TABLE_ADD ? "+" : "-", r->rt_size_change, r->rt_size_new);
        break;
    }
    default:
        break;
    }
}

static void manejador_ip(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "IP del gateway: " IPSTR, IP2STR(&e->ip_info.ip));
    iniciar_sntp();
    sin_router_desde = 0;
    tiene_ip = true;
    probar_guardado = true;                      /* primero, el último broker que anduvo en esta red */
    buscar_broker = true;
}

/* Si el router no vuelve en REINICIO_SIN_ROUTER_MS, reinicia para elegir otra red de la lista */
static void tarea_router(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        TickType_t desde = sin_router_desde;
        if (REINICIO_SIN_ROUTER_MS > 0 && desde != 0 &&
            (xTaskGetTickCount() - desde) >= pdMS_TO_TICKS(REINICIO_SIN_ROUTER_MS)) {
            ESP_LOGW(TAG, "%d min sin router: reinicio para volver a elegir red", REINICIO_SIN_ROUTER_MS / 60000);
            rtc_red_fallida = red_actual;        /* al volver se empieza por la siguiente */
            rtc_magico = RTC_MAGICO;
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
}

/*
 * Elige la red: la primera de REDES_WIFI que esté al alcance en 2,4 GHz (el orden de la
 * lista es la preferencia). Si se reinició porque una red no anduvo, empieza por la
 * siguiente a esa, y la que falló queda última. De la red elegida toma el AP con mejor
 * señal, para conocer su canal y su BSSID. Devuelve el índice en REDES, o -1.
 */
static int buscar_router(uint8_t *canal, uint8_t bssid[6])
{
    int inicio = 0;
    if (rtc_magico == RTC_MAGICO && rtc_red_fallida >= 0 && rtc_red_fallida < (int)CANT_REDES) {
        inicio = (rtc_red_fallida + 1) % CANT_REDES;
        ESP_LOGW(TAG, "La red %s no anduvo: empiezo por la siguiente de la lista", REDES[rtc_red_fallida].ssid);
    }
    rtc_magico = 0;

    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return -1;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *ap = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (ap == NULL) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    esp_wifi_scan_get_ap_records(&n, ap);

    int red = -1, mejor = -1;
    for (int k = 0; k < CANT_REDES && red < 0; k++) {
        int r = (inicio + k) % CANT_REDES;
        for (int i = 0; i < n; i++) {
            if (strcmp((char *)ap[i].ssid, REDES[r].ssid) != 0) {
                continue;
            }
            ESP_LOGI(TAG, "Router visto: %s " MACSTR " canal %d RSSI %d", (char *)ap[i].ssid,
                     MAC2STR(ap[i].bssid), ap[i].primary, ap[i].rssi);
            if (mejor < 0 || ap[i].rssi > ap[mejor].rssi) {
                mejor = i;
                red = r;
            }
        }
    }
    if (red >= 0) {
        *canal = ap[mejor].primary;
        memcpy(bssid, ap[mejor].bssid, 6);
    }
    free(ap);
    return red;
}

void app_main(void)
{
    aura_fw_iniciar();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_read_mac(mi_mac, ESP_MAC_WIFI_STA);
    aura_hw_id_de_mac(mi_mac, mi_hw);
    aura_topico(topico_lwt, sizeof(topico_lwt), mi_mac, "status");
    /* seq arranca al azar: si arrancara siempre en 0, el primer comando tras un reinicio podría
       llevar la misma seq que el último de antes y el que lo recibe lo tomaría por repetido
       (la hoja no lo ejecutaría) */
    seq = (uint16_t)esp_random();
    lock = xSemaphoreCreateMutex();
    nodos_cargar();
    cola_mqtt = xQueueCreate(8, sizeof(mensaje_mqtt_t));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif_ap = NULL;
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, &netif_ap));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &manejador_ip, NULL));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* Argentina: canales 1 a 13 (con el país por defecto, "01", solo 1 a 11). Sin 802.11d. */
    ESP_ERROR_CHECK(esp_wifi_set_country_code("AR", false));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Antes de armar la malla se elige la red y se busca su router para usar su canal */
    uint8_t canal = CANAL_SIN_ROUTER;
    uint8_t bssid[6] = {0};
    red_actual = buscar_router(&canal, bssid);
    bool hay_router = red_actual >= 0;
    const red_wifi_t *red = &REDES[hay_router ? red_actual : 0];
    if (hay_router) {
        ESP_LOGI(TAG, "Red elegida: %s (%d de %d en la lista), canal %d", red->ssid, red_actual + 1,
                 (int)CANT_REDES, canal);
    } else {
        ESP_LOGW(TAG, "No se ve ninguna de las %d redes de la lista en 2,4 GHz; malla en canal %d sin MQTT",
                 (int)CANT_REDES, canal);
    }
    /* Cuenta como "sin router" hasta que tome IP: si la clave está mal, también reinicia y prueba otra */
    sin_router_desde = xTaskGetTickCount() | 1;

    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &manejador_eventos_mesh, NULL));
    ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));     /* este equipo ES la raíz */
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));          /* sin elección de raíz */
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_CAPAS));
    ESP_ERROR_CHECK(esp_mesh_disable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK));

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&cfg.mesh_id, MESH_ID, 6);
    cfg.channel = canal;
    cfg.allow_channel_switch = false;
    cfg.router.ssid_len = strlen(red->ssid);
    memcpy(cfg.router.ssid, red->ssid, cfg.router.ssid_len);
    memcpy(cfg.router.password, red->clave, strlen(red->clave));
    if (hay_router) {
        memcpy(cfg.router.bssid, bssid, 6);            /* fija el BSSID de 2,4 GHz */
    }
    cfg.mesh_ap.max_connection = MESH_MAX_HIJOS;
    cfg.mesh_ap.nonmesh_max_connection = 0;
    memcpy(cfg.mesh_ap.password, MESH_AP_PASS, strlen(MESH_AP_PASS));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_send_block_time(5000));   /* que ningún envío a un nodo se trabe para siempre */
    ESP_ERROR_CHECK(esp_mesh_start());

    xTaskCreate(tarea_recepcion, "mesh_rx", 6144, NULL, 5, NULL);
    xTaskCreate(tarea_bajada, "bajada", 6144, NULL, 5, NULL);
    xTaskCreate(tarea_vigilancia, "vigilancia", 4096, NULL, 4, NULL);
    xTaskCreate(tarea_broker, "broker", 4096, NULL, 4, NULL);
    xTaskCreate(tarea_router, "router", 2048, NULL, 3, NULL);
    ota_gw_iniciar();
    ESP_LOGI(TAG, "Gateway %s listo en canal %d. Esperando nodos...", mi_hw, canal);
}
