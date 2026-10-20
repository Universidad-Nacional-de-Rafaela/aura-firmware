/*
 * ESP-WIFI-MESH de 3 niveles - GATEWAY (ESP32-S3, raíz de la malla)
 *
 * - Es la raíz fija de la malla (capa 1) y además está conectada al router WiFi.
 * - Recibe los JSON que envían los nodos y los publica por MQTT.
 * - El tópico lo decide el nodo: viene en el campo "topic" del JSON.
 *   El gateway lo quita y agrega "via": la MAC del nodo de la malla que se lo entregó
 *   (la propia hoja si manda directo, o su concentrador si se lo reenvía):
 *     recibe   {"topic":"aura/aula1/sensores","id":"aula1-hoja","ts":1792419270346,"temp":24.1}
 *     publica  aura/aula1/sensores  ->  {"id":"aula1-hoja","ts":1792419270346,"temp":24.1,"via":"dc:b4:..."}
 *   Si el mensaje no trae "ts" (el nodo todavía no tiene hora), lo agrega el gateway
 *   con su propia hora y "ts_gw":true.
 * - Bajada: lo que se publique en aura/cmd/<id> se reenvía por la ruta de ese "id",
 *   agregando "dest":"<id>" para que un concentrador sepa a qué hoja pasárselo.
 *   El gateway aprende id -> ruta del último mensaje de cada nodo: una placa
 *   reemplazada o una hoja que pasa a plan B siguen funcionando sin tocar AURA.
 * - Hora: la toma de internet por SNTP y cada PERIODO_HORA_MS la reparte a todos los
 *   nodos conocidos con {"dest":"<id>","hora":<ms desde 1970, UTC>} (también apenas
 *   aparece un nodo o cambia de ruta). Así cada medición sale con su marca de tiempo.
 * - Los JSON se leen y se arman con cJSON (componente espressif/cjson).
 * - Broker: si BROKER_URI (credenciales.h) está vacío, lo busca solo preguntando por
 *   broadcast UDP "AURA_BROKER?" (broker.py contesta). Si pierde el broker
 *   FALLOS_PARA_REBUSCAR veces seguidas, lo vuelve a buscar (p. ej. si cambió la IP).
 * - Es el ÚNICO nodo que depende del WiFi: para cambiar de red se edita
 *   credenciales.h y se regraba solo el gateway.
 *
 * MESH_ID y MESH_AP_PASS deben ser IGUALES en todos los nodos.
 * El canal no es fijo: la malla usa el canal de 2,4 GHz del router.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "credenciales.h"

/* ---------- Configuración común de la malla ---------- */
#define MESH_MAX_HIJOS      6                    /* hijos directos permitidos (máx. 10) */
#define MESH_MAX_CAPAS      6
/* ID y clave de la mesh: en config_local.h (no se sube al repo; plantilla en config_local.ejemplo.h) */
#if __has_include("config_local.h")
#include "config_local.h"
#else
#error "Falta main/config_local.h: copiá config_local.ejemplo.h como config_local.h y completá el ID y la clave de la mesh"
#endif
static const uint8_t MESH_ID[6] = {MESH_ID_BYTES};
/* ----------------------------------------------------- */

#define CANAL_SIN_ROUTER    6                    /* se usa si no se encuentra el router */
#define TOPICO_POR_DEFECTO  "aura/sin_topico"    /* si el JSON no trae "topic" */
#define TOPICO_CMD_PREFIJO  "aura/cmd/"          /* bajada: aura/cmd/<id> -> nodo */
#define SERVIDOR_NTP        "pool.ntp.org"
#define ZONA_HORARIA        "<-03>3"             /* Argentina (UTC-3), solo para los logs */
#define PERIODO_HORA_MS     60000                /* cada cuánto se reparte la hora */
#define PUERTO_DESCUBRIMIENTO 18830              /* UDP; tiene que coincidir con broker.py */
#define FALLOS_PARA_REBUSCAR  3                  /* desconexiones seguidas -> volver a buscar el broker */

static const char *TAG = "GATEWAY";
static uint8_t rx_buf[1500];
static esp_netif_t *netif_sta = NULL;
static esp_mqtt_client_handle_t mqtt = NULL;
static volatile bool mqtt_conectado = false;
static volatile bool tiene_ip = false;
static volatile bool buscar_broker = false;     /* lo atiende tarea_broker */
static volatile int fallos_mqtt = 0;
static volatile bool repartir_hora_ya = false;

/* ------------------------------------------------------------------ */
/*  Hora                                                               */
/* ------------------------------------------------------------------ */
static bool hora_valida(void)
{
    return time(NULL) > 1700000000;              /* posterior a 2023: ya sincronizó */
}

static int64_t ahora_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void hora_sincronizada(struct timeval *tv)
{
    char txt[32];
    struct tm t;
    localtime_r(&tv->tv_sec, &t);
    strftime(txt, sizeof(txt), "%d/%m/%Y %H:%M:%S", &t);
    ESP_LOGI(TAG, "Hora de internet: %s", txt);
    repartir_hora_ya = true;
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
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SERVIDOR_NTP);
    cfg.sync_cb = hora_sincronizada;
    esp_netif_sntp_init(&cfg);                   /* no bloquea; avisa en hora_sincronizada() */
}

/* ------------------------------------------------------------------ */
/*  Tabla id lógico -> ruta (MAC del nodo que entrega sus mensajes)    */
/* ------------------------------------------------------------------ */
/*
 * Cada nodo manda su "id" (p. ej. "aula1-hoja") en todos sus JSON. Con eso el
 * gateway aprende por dónde llegarle. Si se reemplaza una placa o una hoja cambia
 * de padre, su primer mensaje actualiza la ruta: AURA no cambia nada.
 */
#define MAX_NODOS           32
#define LARGO_ID            32

typedef struct {
    char id[LARGO_ID];
    uint8_t mac[6];
} nodo_t;

static nodo_t nodos[MAX_NODOS];
static int cant_nodos = 0;
static portMUX_TYPE nodos_lock = portMUX_INITIALIZER_UNLOCKED;

static void nodo_aprender(const char *id, const uint8_t mac[6])
{
    bool nuevo = false, cambio = false;
    uint8_t mac_vieja[6] = {0};

    taskENTER_CRITICAL(&nodos_lock);
    int i;
    for (i = 0; i < cant_nodos && strcmp(nodos[i].id, id) != 0; i++) {
    }
    if (i == cant_nodos && cant_nodos < MAX_NODOS) {
        strlcpy(nodos[i].id, id, LARGO_ID);
        memcpy(nodos[i].mac, mac, 6);
        cant_nodos++;
        nuevo = true;
    } else if (i < cant_nodos && memcmp(nodos[i].mac, mac, 6) != 0) {
        memcpy(mac_vieja, nodos[i].mac, 6);
        memcpy(nodos[i].mac, mac, 6);
        cambio = true;
    }
    taskEXIT_CRITICAL(&nodos_lock);

    if (nuevo) {
        ESP_LOGI(TAG, "Nodo registrado: %s = " MACSTR, id, MAC2STR(mac));
    } else if (cambio) {
        ESP_LOGW(TAG, "Nodo %s cambió de ruta (otro padre o placa reemplazada): " MACSTR " -> " MACSTR,
                 id, MAC2STR(mac_vieja), MAC2STR(mac));
    }
    if (nuevo || cambio) {
        repartir_hora_ya = true;                 /* que tenga hora cuanto antes */
    }
}

static bool nodo_buscar(const char *id, uint8_t mac[6])
{
    bool ok = false;
    taskENTER_CRITICAL(&nodos_lock);
    for (int i = 0; i < cant_nodos; i++) {
        if (strcmp(nodos[i].id, id) == 0) {
            memcpy(mac, nodos[i].mac, 6);
            ok = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&nodos_lock);
    return ok;
}

/* ------------------------------------------------------------------ */
/*  Envío por la malla                                                 */
/* ------------------------------------------------------------------ */

/* Agrega "dest":"<id>" al JSON y lo manda por la ruta de ese nodo */
static esp_err_t enviar_a_nodo(const char *id, cJSON *json)
{
    mesh_addr_t destino;
    if (!nodo_buscar(id, destino.addr)) {
        ESP_LOGW(TAG, "Nodo desconocido '%s' (todavía no mandó ningún mensaje)", id);
        return ESP_ERR_NOT_FOUND;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(json, "dest");
    cJSON_AddStringToObject(json, "dest", id);

    char *txt = cJSON_PrintUnformatted(json);
    if (txt == NULL) {
        return ESP_ERR_NO_MEM;
    }
    mesh_data_t data = {
        .data = (uint8_t *)txt,
        .size = (uint16_t)strlen(txt),
        .proto = MESH_PROTO_JSON,
        .tos = MESH_TOS_P2P,
    };
    esp_err_t err = esp_mesh_send(&destino, &data, MESH_DATA_P2P, NULL, 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "-> %s (vía " MACSTR ")  %s", id, MAC2STR(destino.addr), txt);
    } else {
        ESP_LOGW(TAG, "No se pudo enviar a %s (" MACSTR "): %s",
                 id, MAC2STR(destino.addr), esp_err_to_name(err));
    }
    cJSON_free(txt);
    return err;
}

/* Reparte la hora a todos los nodos conocidos */
static void tarea_hora(void *arg)
{
    TickType_t ultimo = 0;
    char ids[MAX_NODOS][LARGO_ID];

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        TickType_t t = xTaskGetTickCount();
        if (!hora_valida() || !(repartir_hora_ya || (t - ultimo) >= pdMS_TO_TICKS(PERIODO_HORA_MS))) {
            continue;
        }
        repartir_hora_ya = false;
        ultimo = t;

        int n;
        taskENTER_CRITICAL(&nodos_lock);
        n = cant_nodos;
        for (int i = 0; i < n; i++) {
            strlcpy(ids[i], nodos[i].id, LARGO_ID);
        }
        taskEXIT_CRITICAL(&nodos_lock);

        for (int i = 0; i < n; i++) {
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddNumberToObject(msg, "hora", (double)ahora_ms());
            enviar_a_nodo(ids[i], msg);
            cJSON_Delete(msg);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  MQTT                                                               */
/* ------------------------------------------------------------------ */

/*
 * Orden de bajada: aura/cmd/<id del nodo>, p. ej. aura/cmd/aula1-hoja.
 * El payload tiene que ser un objeto JSON. Se reenvía con "dest" agregado:
 *   {"led":1}  ->  {"led":1,"dest":"aula1-hoja"}
 * El resto de los campos (p. ej. un futuro "prio") pasa sin cambios.
 */
static void reenviar_orden(const char *topico, int topico_len, const char *datos, int datos_len)
{
    const int pref = strlen(TOPICO_CMD_PREFIJO);
    char id[LARGO_ID];
    if (topico_len <= pref || topico_len - pref >= LARGO_ID ||
        strncmp(topico, TOPICO_CMD_PREFIJO, pref) != 0) {
        ESP_LOGW(TAG, "Tópico de orden inválido: %.*s", topico_len, topico);
        return;
    }
    memcpy(id, topico + pref, topico_len - pref);
    id[topico_len - pref] = '\0';

    cJSON *orden = cJSON_ParseWithLength(datos, datos_len);
    if (!cJSON_IsObject(orden)) {
        ESP_LOGW(TAG, "La orden para %s no es un objeto JSON: %.*s", id, datos_len, datos);
        cJSON_Delete(orden);
        return;
    }
    enviar_a_nodo(id, orden);
    cJSON_Delete(orden);
}

static void manejador_mqtt(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_conectado = true;
        fallos_mqtt = 0;
        buscar_broker = false;                 /* volvió solo: no hace falta seguir buscando */
        ESP_LOGI(TAG, "MQTT conectado");
        esp_mqtt_client_subscribe(mqtt, TOPICO_CMD_PREFIJO "+", 0);
        break;
    case MQTT_EVENT_DATA:
        /* Mensajes chicos: llegan completos en un solo evento */
        if (ev->data_len == ev->total_data_len) {
            reenviar_orden(ev->topic, ev->topic_len, ev->data, ev->data_len);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_conectado = false;
        fallos_mqtt++;
        if (BROKER_URI[0] == '\0' && fallos_mqtt >= FALLOS_PARA_REBUSCAR) {
            ESP_LOGW(TAG, "MQTT: %d intentos sin broker, lo vuelvo a buscar", fallos_mqtt);
            buscar_broker = true;              /* ¿cambió la IP de la PC? */
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

/*
 * Pregunta por broadcast "AURA_BROKER?" y espera la respuesta "AURA_BROKER <puerto>"
 * de broker.py. La IP del broker es la de quien contesta. Un intento (~2 s).
 */
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
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),          /* 255.255.255.255 */
    };
    sendto(s, pregunta, strlen(pregunta), 0, (struct sockaddr *)&destino, sizeof(destino));

    /* También al broadcast de la subred (algunos equipos solo atienden ese) */
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif_sta, &ip) == ESP_OK) {
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

/* Busca el broker cuando hace falta y (re)arranca el cliente MQTT con esa dirección */
static void tarea_broker(void *arg)
{
    static char uri[64];
    int intentos = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!tiene_ip || !buscar_broker) {
            continue;
        }
        if (BROKER_URI[0] != '\0') {
            strlcpy(uri, BROKER_URI, sizeof(uri));          /* dirección fija */
        } else if (!descubrir_broker(uri, sizeof(uri))) {
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
            esp_mqtt_client_config_t cfg = {
                .broker.address.uri = uri,
                .credentials.client_id = "aura-gateway",
            };
            mqtt = esp_mqtt_client_init(&cfg);
            esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, manejador_mqtt, NULL);
            esp_mqtt_client_start(mqtt);
        } else {
            esp_mqtt_client_stop(mqtt);
            esp_mqtt_client_set_uri(mqtt, uri);
            esp_mqtt_client_start(mqtt);
        }
    }
}

static void publicar(const char *topico, const char *payload)
{
    if (!mqtt_conectado) {
        ESP_LOGW(TAG, "Sin MQTT, se descarta el mensaje para %s", topico);
        return;
    }
    if (esp_mqtt_client_publish(mqtt, topico, payload, 0, 0, 0) >= 0) {
        ESP_LOGI(TAG, "MQTT -> %s  %s", topico, payload);
    } else {
        ESP_LOGW(TAG, "No se pudo publicar en %s", topico);
    }
}

/* ------------------------------------------------------------------ */
/*  Malla: lo que suben los nodos                                      */
/* ------------------------------------------------------------------ */
static void tarea_recepcion(void *arg)
{
    mesh_addr_t origen;
    mesh_data_t data;
    int flag = 0;
    char via[18];

    while (true) {
        data.data = rx_buf;
        data.size = sizeof(rx_buf) - 1;

        esp_err_t err = esp_mesh_recv(&origen, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Error de recepción: %s", esp_err_to_name(err));
            continue;
        }
        rx_buf[data.size] = '\0';
        printf("[" MACSTR "] %s\n", MAC2STR(origen.addr), (char *)rx_buf);

        cJSON *json = cJSON_Parse((char *)rx_buf);
        if (!cJSON_IsObject(json)) {
            /* No es JSON válido: se publica tal cual para poder verlo */
            publicar(TOPICO_POR_DEFECTO, (char *)rx_buf);
            cJSON_Delete(json);
            continue;
        }

        const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
        if (cJSON_IsString(id) && id->valuestring[0] != '\0' && strlen(id->valuestring) < LARGO_ID) {
            nodo_aprender(id->valuestring, origen.addr);
        }

        /* El tópico lo elige el nodo; se saca del payload */
        char topico[128];
        cJSON *t = cJSON_DetachItemFromObjectCaseSensitive(json, "topic");
        if (cJSON_IsString(t) && t->valuestring[0] != '\0' && strlen(t->valuestring) < sizeof(topico)) {
            strlcpy(topico, t->valuestring, sizeof(topico));
        } else {
            strlcpy(topico, TOPICO_POR_DEFECTO, sizeof(topico));
        }
        cJSON_Delete(t);

        /* Marca de tiempo: la del nodo, o la del gateway si el nodo todavía no tiene hora */
        if (!cJSON_HasObjectItem(json, "ts") && hora_valida()) {
            cJSON_AddNumberToObject(json, "ts", (double)ahora_ms());
            cJSON_AddTrueToObject(json, "ts_gw");
        }
        snprintf(via, sizeof(via), MACSTR, MAC2STR(origen.addr));
        cJSON_AddStringToObject(json, "via", via);

        char *payload = cJSON_PrintUnformatted(json);
        if (payload != NULL) {
            publicar(topico, payload);
            cJSON_free(payload);
        }
        cJSON_Delete(json);
    }
}

static void manejador_eventos_mesh(void *arg, esp_event_base_t base,
                                   int32_t id, void *event_data)
{
    switch (id) {
    case MESH_EVENT_STARTED:
        ESP_LOGI(TAG, "Malla iniciada. Raíz: %s, capa %d",
                 esp_mesh_is_root() ? "sí" : "no", esp_mesh_get_layer());
        break;

    case MESH_EVENT_PARENT_CONNECTED: {
        /* En la raíz, el "padre" es el router: se pide IP por DHCP */
        mesh_event_connected_t *c = (mesh_event_connected_t *)event_data;
        ESP_LOGI(TAG, "Conectado al router " MACSTR " (canal %d)",
                 MAC2STR(c->connected.bssid), c->connected.channel);
        if (esp_mesh_is_root()) {
            esp_netif_dhcpc_stop(netif_sta);
            esp_netif_dhcpc_start(netif_sta);
        }
        break;
    }
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *d = (mesh_event_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Sin conexión al router (motivo %d), reintentando...", d->reason);
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
                 id == MESH_EVENT_ROUTING_TABLE_ADD ? "+" : "-",
                 r->rt_size_change, r->rt_size_new);
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
    tiene_ip = true;
    buscar_broker = true;                        /* IP nueva (quizás otra red): buscar el broker */
}

/* Busca el router en 2,4 GHz para conocer su canal y BSSID */
static bool buscar_router(uint8_t *canal, uint8_t bssid[6])
{
    wifi_scan_config_t sc = {
        .ssid = (uint8_t *)ROUTER_SSID,
        .show_hidden = false,
    };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return false;
    }
    uint16_t n = 10;
    wifi_ap_record_t ap[10];
    esp_wifi_scan_get_ap_records(&n, ap);

    int mejor = -1;
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "Router visto: %s " MACSTR " canal %d RSSI %d",
                 (char *)ap[i].ssid, MAC2STR(ap[i].bssid), ap[i].primary, ap[i].rssi);
        if (mejor < 0 || ap[i].rssi > ap[mejor].rssi) {
            mejor = i;
        }
    }
    if (mejor < 0) {
        return false;
    }
    *canal = ap[mejor].primary;
    memcpy(bssid, ap[mejor].bssid, 6);
    return true;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif_ap = NULL;
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, &netif_ap));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &manejador_ip, NULL));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Antes de armar la malla se busca el router para usar su canal */
    uint8_t canal = CANAL_SIN_ROUTER;
    uint8_t bssid[6] = {0};
    bool hay_router = buscar_router(&canal, bssid);
    if (hay_router) {
        ESP_LOGI(TAG, "Router %s en canal %d", ROUTER_SSID, canal);
    } else {
        ESP_LOGW(TAG, "No se encontró %s en 2,4 GHz; malla en canal %d sin MQTT",
                 ROUTER_SSID, canal);
    }

    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                               &manejador_eventos_mesh, NULL));

    ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));     /* este equipo ES la raíz */
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));          /* sin elección de raíz */
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_CAPAS));
    ESP_ERROR_CHECK(esp_mesh_disable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK));

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&cfg.mesh_id, MESH_ID, 6);
    cfg.channel = canal;
    cfg.allow_channel_switch = false;

    cfg.router.ssid_len = strlen(ROUTER_SSID);
    memcpy(cfg.router.ssid, ROUTER_SSID, cfg.router.ssid_len);
    memcpy(cfg.router.password, ROUTER_PASS, strlen(ROUTER_PASS));
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
    xTaskCreate(tarea_hora, "hora", 4096, NULL, 4, NULL);
    xTaskCreate(tarea_broker, "broker", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Gateway listo en canal %d. Esperando nodos...", canal);
}
