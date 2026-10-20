/*
 * ESP-WIFI-MESH de 3 niveles - NODO HOJA (ESP32-C3 SuperMini, capa 3)
 *
 * Elección de padre (sin ninguna MAC en el firmware):
 * - MODO AULA (normal): se cuelga del concentrador que anuncia SU aula en el beacon
 *   (vendor IE de AURA) y le manda cada mensaje UNA sola vez. El concentrador lo
 *   muestra y lo reenvía a la raíz; las órdenes de AURA le llegan a través de él.
 * - MODO DIRECTO (plan B): si no encuentra a su concentrador en BUSQUEDAS_PLAN_B
 *   escaneos seguidos, se cuelga de la raíz o de cualquier otro concentrador (nunca
 *   de otra hoja) y manda directo a la raíz, así AURA sigue enterada.
 * - En modo directo, cada REVISAR_AULA_MS escanea solo el canal actual (rápido, sin
 *   cortar la conexión); si su concentrador volvió, se pasa a él.
 * Si se reemplaza el concentrador o esta hoja por otra placa con el mismo firmware,
 * todo sigue funcionando: no hay nada que reprogramar.
 *
 * Mensajes (se arman y se leen con cJSON, componente espressif/cjson):
 * - Cada 2 s, datos simulados + estado del LED (así la pantalla y AURA se
 *   resincronizan solos). "topic" = tópico MQTT, "id" = nombre lógico en AURA,
 *   "ts" = hora de la medición en ms UTC (cuando ya la recibió del gateway).
 * - Órdenes: {"led":1} {"led":0} {"led":"toggle"}, desde AURA (MQTT aura/cmd/<id>)
 *   o desde el botón del concentrador ("origen":"local"). Responde con el estado real.
 * - Hora: {"hora":<ms UTC>}, que el gateway reparte cada minuto.
 *
 * MESH_ID y MESH_AP_PASS deben coincidir con el gateway.
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
#include "esp_random.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "cJSON.h"

/* ---------- Configuración común de la malla (igual que el gateway) ---------- */
#define MESH_MAX_CAPAS      6
/* ID y clave de la mesh: en config_local.h (no se sube al repo; plantilla en config_local.ejemplo.h) */
#if __has_include("config_local.h")
#include "config_local.h"
#else
#error "Falta main/config_local.h: copiá config_local.ejemplo.h como config_local.h y completá el ID y la clave de la mesh"
#endif
static const uint8_t MESH_ID[6] = {MESH_ID_BYTES};
/* La API de la malla exige un SSID de router, pero este nodo nunca se conecta al router:
   solo el gateway depende del WiFi. Es un valor fijo que NO hay que cambiar al mudar la red. */
#define ROUTER_FICTICIO     "aura-sin-router"
/* ---------------------------------------------------------------------------- */

/* ---------- Identidad de este equipo (lo único que cambia entre aulas) ---------- */
#define AULA                "aula1"
#define DEVICE_ID           AULA "-hoja"
#define TOPICO_DATOS        "aura/" AULA "/sensores"
#define TOPICO_LED          "aura/" AULA "/led"
/* --------------------------------------------------------------------------------- */

/* Vendor IE de AURA que anuncia cada concentrador (ver Mesh3_Intermedio) */
#define AURA_OUI_0          0x02
#define AURA_OUI_1          0xA5
#define AURA_OUI_2          0x52
#define AURA_IE_CONCENTRADOR 0x01

#define PIN_LED             GPIO_NUM_8         /* LED azul de la C3 SuperMini */
#define LED_ACTIVO_BAJO     1                  /* se enciende con 0 */

#define ESPERA_CONEXION_MS  15000              /* sin conectar tras elegir padre -> reescanear */
#define ESPERA_RECONEXION_MS 5000              /* tras perder el padre, cuánto se le deja volver solo */
#define BUSQUEDAS_PLAN_B    3                  /* escaneos sin ver al concentrador -> plan B */
#define REVISAR_AULA_MS     30000              /* en plan B, cada cuánto ver si volvió */
#define LIMITE_ENVIO_MS     5000               /* tiempo máximo de un esp_mesh_send() */
#define FALLOS_PARA_RECONECTAR 3               /* envíos fallidos seguidos -> reconexión forzada */

/* Potencia de transmisión, en pasos de 0,25 dBm (40 = 10 dBm, 60 = 15 dBm, 78 = 19,5 dBm, 84 = 21 dBm).
   La C3 SuperMini tiene la antena mal adaptada: con la potencia de fábrica no se conectaba,
   y a 10 dBm anda pero con poco alcance. Otras placas pueden ir al máximo. */
#define POTENCIA_TX         60                 /* 15 dBm: con antena de alambre de 31 mm, ~10 m a través de una vidriera (probado) */

typedef enum { MODO_AULA, MODO_DIRECTO } modo_t;

static const char *TAG = "HOJA";
static volatile bool conectado = false;
static volatile bool escaneando = false;
static volatile TickType_t ultimo_intento = 0;
static volatile TickType_t ultima_revision = 0;
static volatile TickType_t desconectada_desde = 0;
static volatile bool reintentando = false;     /* la pila mesh reintenta solo el padre perdido */
static volatile modo_t modo = MODO_AULA;       /* modo de la conexión actual */
static volatile modo_t modo_pedido = MODO_AULA;/* modo del último padre elegido */
static int busquedas_fallidas = 0;
static volatile int fallos_envio = 0;
static bool led = false;

/* Concentradores vistos en los beacons: BSSID del AP -> MAC en la malla, y si es de mi aula */
#define MAX_CANDIDATOS      8
typedef struct {
    uint8_t bssid[6];
    uint8_t mac_malla[6];
    bool de_mi_aula;
} candidato_t;
static candidato_t candidatos[MAX_CANDIDATOS];
static int cant_candidatos = 0;
static portMUX_TYPE candidatos_lock = portMUX_INITIALIZER_UNLOCKED;

/* Concentrador de mi aula (destino de los mensajes en modo aula) */
static mesh_addr_t concentrador;
static mesh_addr_t concentrador_pedido;

/* ------------------------------------------------------------------ */
/*  LED y envío                                                        */
/* ------------------------------------------------------------------ */
static void led_aplicar(bool encendido)
{
    led = encendido;
    gpio_set_level(PIN_LED, LED_ACTIVO_BAJO ? !encendido : encendido);
}

/*
 * Señales de vida en el LED (para ver qué pasa sin la PC, p. ej. con un cargador):
 *   2 destellos cortos = arrancó (si se repiten cada pocos segundos, se reinicia en bucle)
 *   1 destello largo   = se conectó a su padre
 * Después vuelve al estado que tenía el LED.
 */
static void destellos(int veces, int ms)
{
    for (int i = 0; i < veces; i++) {
        gpio_set_level(PIN_LED, LED_ACTIVO_BAJO ? led : !led);   /* invertido */
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level(PIN_LED, LED_ACTIVO_BAJO ? !led : led);   /* estado real */
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

static volatile bool avisar_conexion = false;

static void tarea_senales(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (avisar_conexion) {
            avisar_conexion = false;
            destellos(1, 1000);
        }
    }
}

/* Motivo del último reinicio, en texto (va en el primer mensaje después de arrancar) */
static const char *motivo_reinicio(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "encendido";
    case ESP_RST_BROWNOUT:  return "brownout (cayó la alimentación)";
    case ESP_RST_PANIC:     return "panic (error de firmware)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_SW:        return "reinicio por software";
    case ESP_RST_EXT:       return "pin de reset";
    case ESP_RST_USB:       return "USB";
    default:                return "otro";
    }
}

static const char *modo_txt(void)
{
    return modo == MODO_AULA ? "aula" : "directo";
}

/* ------------------------------------------------------------------ */
/*  Hora (la reparte el gateway con {"hora":<ms UTC>})                 */
/* ------------------------------------------------------------------ */
static bool hora_valida(void)
{
    return time(NULL) > 1700000000;            /* posterior a 2023: ya la recibió */
}

static int64_t ahora_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void hora_ajustar(double ms)
{
    bool primera = !hora_valida();
    struct timeval tv = {
        .tv_sec = (time_t)(ms / 1000),
        .tv_usec = (suseconds_t)((int64_t)ms % 1000) * 1000,
    };
    settimeofday(&tv, NULL);
    if (primera) {
        ESP_LOGI(TAG, "Hora recibida del gateway: los mensajes ya salen con \"ts\"");
    }
}

/*
 * Campos que van en TODOS los mensajes de esta hoja: tópico, id, MAC, modo y,
 * si ya tiene hora, la marca de tiempo. Quien llama agrega el resto y libera el objeto.
 */
static char mac_txt[18];                       /* MAC propia en texto (se carga en app_main) */

static cJSON *mensaje_nuevo(const char *topico)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "topic", topico);
    cJSON_AddStringToObject(msg, "id", DEVICE_ID);
    cJSON_AddStringToObject(msg, "mac", mac_txt);
    cJSON_AddStringToObject(msg, "modo", modo_txt());
    if (hora_valida()) {
        cJSON_AddNumberToObject(msg, "ts", (double)ahora_ms());
    }
    return msg;
}

/* Un solo envío: al concentrador en modo aula, a la raíz en modo directo */
static void publicar(const char *msg)
{
    bool via_aula = (modo == MODO_AULA);
    mesh_data_t data = {
        .data = (uint8_t *)msg,
        .size = (uint16_t)strlen(msg),
        .proto = MESH_PROTO_JSON,
        .tos = MESH_TOS_P2P,
    };
    esp_err_t err = esp_mesh_send(via_aula ? &concentrador : NULL, &data, MESH_DATA_P2P, NULL, 0);
    if (err == ESP_OK) {
        fallos_envio = 0;
        return;
    }
    ESP_LOGW(TAG, "Error al enviar a %s: %s", via_aula ? "concentrador" : "raíz",
             esp_err_to_name(err));
    /* Vigilante de envío: si el padre se reinició muy rápido, la hoja puede quedar
       "conectada" con el control de flujo de la malla trabado ([WND-RX] timeout) y
       ningún envío sale. Se fuerza la desconexión y la búsqueda normal de padre. */
    if (conectado && ++fallos_envio >= FALLOS_PARA_RECONECTAR) {
        ESP_LOGW(TAG, "%d envíos fallidos seguidos: fuerzo la reconexión", fallos_envio);
        fallos_envio = 0;
        esp_mesh_disconnect();
    }
}

/* Convierte el objeto a texto, lo publica y lo libera */
static void publicar_json(cJSON *msg)
{
    char *txt = cJSON_PrintUnformatted(msg);
    if (txt != NULL) {
        publicar(txt);
        ESP_LOGI(TAG, "Enviado (%s): %s", modo_txt(), txt);
        cJSON_free(txt);
    }
    cJSON_Delete(msg);
}

/* ------------------------------------------------------------------ */
/*  Elección del padre                                                 */
/* ------------------------------------------------------------------ */

/* Se llama por cada beacon/probe response con vendor IE (tarea WiFi: debe ser breve) */
static void vendor_ie_cb(void *ctx, wifi_vendor_ie_type_t tipo, const uint8_t sa[6],
                         const vendor_ie_data_t *ie, int rssi)
{
    if (ie->vendor_oui[0] != AURA_OUI_0 || ie->vendor_oui[1] != AURA_OUI_1 ||
        ie->vendor_oui[2] != AURA_OUI_2 || ie->vendor_oui_type != AURA_IE_CONCENTRADOR ||
        ie->length < 4 + 6) {
        return;                                /* no es un concentrador de AURA */
    }
    const size_t largo_aula = strlen(AULA);
    bool mia = (ie->length == 4 + 6 + largo_aula) && memcmp(ie->payload + 6, AULA, largo_aula) == 0;

    taskENTER_CRITICAL(&candidatos_lock);
    int i;
    for (i = 0; i < cant_candidatos && memcmp(candidatos[i].bssid, sa, 6) != 0; i++) {
    }
    if (i < MAX_CANDIDATOS) {
        memcpy(candidatos[i].bssid, sa, 6);
        memcpy(candidatos[i].mac_malla, ie->payload, 6);
        candidatos[i].de_mi_aula = mia;
        if (i == cant_candidatos) {
            cant_candidatos++;
        }
    }
    taskEXIT_CRITICAL(&candidatos_lock);
}

/* ¿Ese BSSID anunció ser un concentrador? Devuelve 0 = no, 1 = de otra aula, 2 = de mi aula */
static int tipo_concentrador(const uint8_t bssid[6], uint8_t mac_malla[6])
{
    int tipo = 0;
    taskENTER_CRITICAL(&candidatos_lock);
    for (int i = 0; i < cant_candidatos; i++) {
        if (memcmp(candidatos[i].bssid, bssid, 6) == 0) {
            memcpy(mac_malla, candidatos[i].mac_malla, 6);
            tipo = candidatos[i].de_mi_aula ? 2 : 1;
            break;
        }
    }
    taskEXIT_CRITICAL(&candidatos_lock);
    return tipo;
}

/* solo_canal_actual: escaneo rápido sin perder la conexión (para el chequeo en plan B) */
static void iniciar_escaneo(bool solo_canal_actual)
{
    wifi_scan_config_t sc = {
        .show_hidden = true,                   /* el AP de la malla es oculto */
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time.passive = 600,              /* ms por canal: el beacon de la malla sale cada 300 ms */
    };
    if (solo_canal_actual) {
        wifi_second_chan_t sec;
        esp_wifi_get_channel(&sc.channel, &sec);
    }
    taskENTER_CRITICAL(&candidatos_lock);
    cant_candidatos = 0;
    taskEXIT_CRITICAL(&candidatos_lock);

    esp_wifi_scan_stop();
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
        escaneando = true;
        if (!solo_canal_actual) {
            ESP_LOGI(TAG, "Buscando al concentrador de %s...", AULA);
        }
    }
}

static void conectar(const wifi_ap_record_t *rec, const mesh_assoc_t *assoc,
                     const uint8_t *mac_malla, modo_t nuevo_modo)
{
    wifi_config_t padre = {0};
    padre.sta.channel = rec->primary;
    memcpy(padre.sta.ssid, rec->ssid, sizeof(rec->ssid));
    padre.sta.bssid_set = 1;
    memcpy(padre.sta.bssid, rec->bssid, 6);
    esp_mesh_set_ap_authmode(rec->authmode);
    if (rec->authmode != WIFI_AUTH_OPEN) {
        memcpy(padre.sta.password, MESH_AP_PASS, strlen(MESH_AP_PASS));
    }

    modo_pedido = nuevo_modo;
    if (nuevo_modo == MODO_AULA) {
        memcpy(concentrador_pedido.addr, mac_malla, 6);
    }
    ESP_LOGI(TAG, "Padre elegido (%s): %s " MACSTR " capa %d, canal %d, RSSI %d",
             nuevo_modo == MODO_AULA ? "MODO AULA, mi concentrador" : "PLAN B, envío directo",
             (char *)padre.sta.ssid, MAC2STR(rec->bssid), assoc->layer, rec->primary, rec->rssi);
    ultimo_intento = xTaskGetTickCount();
    /* En IDF 6.1 set_parent rechaza MESH_LEAF: se usa MESH_NODE (ver app_main) */
    esp_err_t err = esp_mesh_set_parent(&padre, (mesh_addr_t *)&assoc->mesh_id,
                                        MESH_NODE, assoc->layer + 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_mesh_set_parent: %s (0x%x)", esp_err_to_name(err), err);
    }
}

static void procesar_escaneo(int num)
{
    mesh_assoc_t assoc, mio_assoc, alt_assoc;
    wifi_ap_record_t rec, mio_rec, alt_rec;
    uint8_t mac_malla[6], mio_mac[6];
    bool hay_mio = false, hay_alt = false;
    int ie_len = 0;

    for (int i = 0; i < num; i++) {
        esp_mesh_scan_get_ap_ie_len(&ie_len);
        esp_mesh_scan_get_ap_record(&rec, &assoc);
        if (ie_len != sizeof(assoc)) {
            continue;                          /* no es un nodo de malla */
        }
        int tipo = tipo_concentrador(rec.bssid, mac_malla);
        bool en_malla = assoc.layer >= 1 && assoc.layer < MESH_MAX_CAPAS;
        if (!conectado || modo == MODO_DIRECTO) {
            ESP_LOGI(TAG, "  malla: " MACSTR " capa %d canal %d RSSI %d %s",
                     MAC2STR(rec.bssid), assoc.layer, rec.primary, rec.rssi,
                     tipo == 2 ? "<- concentrador de " AULA :
                     tipo == 1 ? "(concentrador de otra aula)" :
                     assoc.layer == 1 ? "(raíz)" : "");
        }
        if (!en_malla) {
            continue;
        }
        /* Mi concentrador: el de mejor señal (si hubiera dos, p. ej. durante un reemplazo) */
        if (tipo == 2 && (!hay_mio || rec.rssi > mio_rec.rssi)) {
            mio_rec = rec;
            mio_assoc = assoc;
            memcpy(mio_mac, mac_malla, 6);
            hay_mio = true;
        }
        /* Plan B: la raíz u otro concentrador con lugar, nunca una hoja.
           Preferencia: menor capa y, a igual capa, mejor señal. */
        if ((assoc.layer == 1 || tipo == 1) && assoc.assoc < assoc.assoc_cap &&
            (!hay_alt || assoc.layer < alt_assoc.layer ||
             (assoc.layer == alt_assoc.layer && rec.rssi > alt_rec.rssi))) {
            alt_rec = rec;
            alt_assoc = assoc;
            hay_alt = true;
        }
    }
    esp_mesh_flush_scan_result();
    escaneando = false;

    if (conectado) {
        /* Chequeo periódico en plan B: ¿volvió mi concentrador? */
        if (modo == MODO_DIRECTO && hay_mio) {
            ESP_LOGI(TAG, "Volvió el concentrador de %s: dejo el plan B", AULA);
            busquedas_fallidas = 0;
            conectar(&mio_rec, &mio_assoc, mio_mac, MODO_AULA);
        }
        return;
    }

    if (hay_mio) {
        busquedas_fallidas = 0;
        conectar(&mio_rec, &mio_assoc, mio_mac, MODO_AULA);
        return;
    }
    busquedas_fallidas++;
    if (busquedas_fallidas < BUSQUEDAS_PLAN_B) {
        ESP_LOGW(TAG, "El concentrador de %s no está visible (%d/%d), se reintenta",
                 AULA, busquedas_fallidas, BUSQUEDAS_PLAN_B);
    } else if (hay_alt) {
        conectar(&alt_rec, &alt_assoc, NULL, MODO_DIRECTO);
    } else {
        ESP_LOGW(TAG, "Sin concentrador de %s ni otro padre posible, se reintenta", AULA);
    }
}

/* Reintenta la búsqueda mientras no esté conectada, y en plan B revisa si volvió su concentrador */
static void tarea_vigia(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (escaneando) {
            continue;
        }
        TickType_t ahora = xTaskGetTickCount();
        if (conectado) {
            if (modo == MODO_DIRECTO && (ahora - ultima_revision) >= pdMS_TO_TICKS(REVISAR_AULA_MS)) {
                ultima_revision = ahora;
                iniciar_escaneo(true);
            }
            continue;
        }
        if (ultimo_intento != 0 && (ahora - ultimo_intento) < pdMS_TO_TICKS(ESPERA_CONEXION_MS)) {
            continue;                          /* se está intentando conectar */
        }
        /* Con la auto-organización apagada, la pila reintenta sin fin el último padre
           pedido y mientras tanto el WiFi no deja escanear. Si se perdió un padre se le
           da un margen por si fue un corte breve; después (o si el padre elegido nunca
           respondió) se cortan los reintentos para poder buscar otro. */
        if (reintentando && (ahora - desconectada_desde) < pdMS_TO_TICKS(ESPERA_RECONEXION_MS)) {
            continue;
        }
        if (reintentando || ultimo_intento != 0) {
            ESP_LOGW(TAG, "El padre no responde: dejo de reintentar y busco otro");
            esp_mesh_set_self_organized(false, false);
            reintentando = false;
            ultimo_intento = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        iniciar_escaneo(false);
    }
}

/* ------------------------------------------------------------------ */
/*  Datos y órdenes                                                    */
/* ------------------------------------------------------------------ */
static void tarea_envio(void *arg)
{
    uint32_t contador = 0;

    while (true) {
        if (conectado) {
            /* Datos simulados. En double y con un decimal para que cJSON los escriba
               tal cual (24.1 y no 24.100000381). */
            double temp = 20.0 + (double)(esp_random() % 100) / 10.0;   /* 20,0 a 29,9 */
            double hum = 40.0 + (double)(esp_random() % 300) / 10.0;    /* 40,0 a 69,9 */
            bool presencia = (esp_random() % 2) == 1;

            cJSON *msg = mensaje_nuevo(TOPICO_DATOS);
            if (contador == 0) {
                cJSON_AddStringToObject(msg, "reinicio", motivo_reinicio());
            }
            cJSON_AddNumberToObject(msg, "n", contador++);
            cJSON_AddNumberToObject(msg, "capa", esp_mesh_get_layer());
            wifi_ap_record_t padre;
            if (esp_wifi_sta_get_ap_info(&padre) == ESP_OK) {
                cJSON_AddNumberToObject(msg, "rssi", padre.rssi);   /* señal del padre, en dBm */
            }
            cJSON_AddNumberToObject(msg, "temp", temp);
            cJSON_AddNumberToObject(msg, "hum", hum);
            cJSON_AddBoolToObject(msg, "pres", presencia);
            cJSON_AddNumberToObject(msg, "led", led ? 1 : 0);
            publicar_json(msg);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/*
 * Interpreta el campo "led" de una orden. Acepta 1/0, true/false, "on"/"off" y
 * "toggle". Devuelve false si la orden no trae "led" o trae un valor desconocido.
 */
static bool leer_orden_led(const cJSON *orden, bool *nuevo)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(orden, "led");
    if (cJSON_IsNumber(v)) {
        *nuevo = v->valueint != 0;
    } else if (cJSON_IsBool(v)) {
        *nuevo = cJSON_IsTrue(v);
    } else if (cJSON_IsString(v) && strcmp(v->valuestring, "toggle") == 0) {
        *nuevo = !led;
    } else if (cJSON_IsString(v) && strcmp(v->valuestring, "on") == 0) {
        *nuevo = true;
    } else if (cJSON_IsString(v) && strcmp(v->valuestring, "off") == 0) {
        *nuevo = false;
    } else {
        return false;
    }
    return true;
}

static void tarea_ordenes(void *arg)
{
    static uint8_t buf[600];
    uint32_t contador = 0;
    mesh_addr_t origen;
    mesh_data_t data;
    int flag = 0;

    while (true) {
        data.data = buf;
        data.size = sizeof(buf) - 1;
        if (esp_mesh_recv(&origen, &data, portMAX_DELAY, &flag, NULL, 0) != ESP_OK) {
            continue;
        }
        buf[data.size] = '\0';

        cJSON *orden = cJSON_Parse((const char *)buf);
        if (!cJSON_IsObject(orden)) {
            ESP_LOGW(TAG, "Mensaje que no es JSON de " MACSTR ": %s", MAC2STR(origen.addr), (char *)buf);
            cJSON_Delete(orden);
            continue;
        }

        /* Hora que reparte el gateway */
        const cJSON *hora = cJSON_GetObjectItemCaseSensitive(orden, "hora");
        if (cJSON_IsNumber(hora)) {
            hora_ajustar(hora->valuedouble);
        }

        /* Orden para el LED */
        bool nuevo;
        if (leer_orden_led(orden, &nuevo)) {
            ESP_LOGI(TAG, "Orden recibida de " MACSTR ": %s", MAC2STR(origen.addr), (char *)buf);
            led_aplicar(nuevo);

            /* "origen" lo pone quien manda la orden; si no viene, es de AURA */
            const cJSON *o = cJSON_GetObjectItemCaseSensitive(orden, "origen");
            bool local = cJSON_IsString(o) && strcmp(o->valuestring, "local") == 0;

            cJSON *resp = mensaje_nuevo(TOPICO_LED);
            cJSON_AddNumberToObject(resp, "led", led ? 1 : 0);
            cJSON_AddStringToObject(resp, "origen", local ? "local" : "aura");
            cJSON_AddNumberToObject(resp, "n", contador++);
            publicar_json(resp);
        } else if (!cJSON_IsNumber(hora)) {
            ESP_LOGW(TAG, "Orden no reconocida: %s", (char *)buf);
        }
        cJSON_Delete(orden);
    }
}

/* ------------------------------------------------------------------ */
static void manejador_eventos_mesh(void *arg, esp_event_base_t base,
                                   int32_t id, void *event_data)
{
    switch (id) {
    case MESH_EVENT_STARTED:
        /* La hoja no elige padre sola: lo hace procesar_escaneo() */
        esp_mesh_set_self_organized(false, false);
        iniciar_escaneo(false);
        break;
    case MESH_EVENT_SCAN_DONE: {
        mesh_event_scan_done_t *s = (mesh_event_scan_done_t *)event_data;
        procesar_escaneo(s->number);
        break;
    }
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *c = (mesh_event_connected_t *)event_data;
        if (modo_pedido == MODO_AULA) {
            memcpy(concentrador.addr, concentrador_pedido.addr, 6);
        }
        modo = modo_pedido;
        conectado = true;
        avisar_conexion = true;                /* destello largo en el LED */
        ultima_revision = xTaskGetTickCount();
        ESP_LOGI(TAG, "Conectada a " MACSTR " - capa %d - %s",
                 MAC2STR(c->connected.bssid), esp_mesh_get_layer(),
                 modo == MODO_AULA ? "MODO AULA (todo pasa por el concentrador)"
                                   : "PLAN B (envío directo a la raíz)");
        break;
    }
    case MESH_EVENT_PARENT_DISCONNECTED:
        if (conectado) {
            ESP_LOGW(TAG, "Desconectada del padre");
            desconectada_desde = xTaskGetTickCount();
            reintentando = true;
        }
        conectado = false;
        break;
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

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_txt, sizeof(mac_txt), MACSTR, MAC2STR(mac));

    gpio_reset_pin(PIN_LED);
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    led_aplicar(false);
    ESP_LOGI(TAG, "Arranque. Motivo del último reinicio: %s", motivo_reinicio());
    destellos(2, 150);                                 /* señal de vida: arrancó */
    xTaskCreate(tarea_senales, "senales", 2048, NULL, 3, NULL);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif_sta = NULL;
    esp_netif_t *netif_ap = NULL;
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, &netif_ap));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_vendor_ie_cb(vendor_ie_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_max_tx_power(POTENCIA_TX);
    int8_t potencia;
    esp_wifi_get_max_tx_power(&potencia);
    ESP_LOGI(TAG, "Potencia de transmisión: %.2f dBm", potencia / 4.0);

    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                               &manejador_eventos_mesh, NULL));

    /* En IDF 6.1 esp_mesh_set_parent() rechaza MESH_LEAF (devuelve ESP_FAIL), con o
       sin esp_mesh_set_type(MESH_LEAF). Se une como MESH_NODE y casi no acepta hijos
       (mesh_ap.max_connection = 1, el mínimo que admite la API). Otras hojas no la
       eligen como padre: en plan B solo se usan la raíz o los concentradores. */
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_CAPAS));
    ESP_ERROR_CHECK(esp_mesh_disable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK));

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&cfg.mesh_id, MESH_ID, 6);
    cfg.channel = 0;                                   /* el canal sale del escaneo */
    cfg.allow_channel_switch = true;

    cfg.router.ssid_len = strlen(ROUTER_FICTICIO);
    memcpy(cfg.router.ssid, ROUTER_FICTICIO, cfg.router.ssid_len);

    cfg.mesh_ap.max_connection = 1;
    cfg.mesh_ap.nonmesh_max_connection = 0;
    memcpy(cfg.mesh_ap.password, MESH_AP_PASS, strlen(MESH_AP_PASS));

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_send_block_time(LIMITE_ENVIO_MS));   /* que ningún envío se trabe para siempre */
    ESP_ERROR_CHECK(esp_mesh_start());

    ESP_LOGI(TAG, "Hoja '%s' iniciada. Órdenes por MQTT en aura/cmd/%s", DEVICE_ID, DEVICE_ID);

    xTaskCreate(tarea_vigia, "vigia", 3072, NULL, 4, NULL);
    xTaskCreate(tarea_envio, "mesh_tx", 4096, NULL, 5, NULL);
    xTaskCreate(tarea_ordenes, "mesh_cmd", 4096, NULL, 5, NULL);
}
