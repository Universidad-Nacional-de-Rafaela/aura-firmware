/*
 * ESP-WIFI-MESH de 3 niveles - CONCENTRADOR DEL AULA (ESP32-S3, capa 2)
 *
 * - Se une a la raíz fija (gateway) y acepta hijos: las hojas del aula se cuelgan de él.
 * - Anuncia en su beacon WiFi un "vendor IE" con su aula y su MAC. La hoja elige
 *   como padre al concentrador de SU aula, no a una MAC fija: si se reemplaza esta
 *   placa por otra con el mismo firmware, la hoja la encuentra igual.
 * - Reenvío del aula (un solo mensaje por dato dentro del aula):
 *     subida:  hoja -> concentrador (lo muestra) -> raíz -> MQTT
 *     bajada:  MQTT -> raíz -> concentrador -> hoja, según el campo "dest" que agrega el gateway
 *   La tabla id -> MAC de las hojas se aprende de sus mensajes.
 *   Si el concentrador se cae, las hojas pasan solas a plan B (mandan directo a la raíz).
 * - "Pantalla" (por ahora el LED RGB y el puerto serie): el LED muestra el estado que
 *   INFORMA la hoja (en sus respuestas y en cada mensaje periódico).
 * - Mando local: el botón BOOT (GPIO0) manda {"led":"toggle"} directo a la hoja,
 *   sin pasar por el gateway ni por MQTT.
 * - Cada 10 s envía su estado a la raíz (tópico aura/<aula>/concentrador/estado).
 * - Hora: la recibe del gateway ({"dest":"<id>","hora":<ms UTC>}) y la usa para el "ts"
 *   de sus mensajes; la de las hojas se la pasa como cualquier otro mensaje con "dest".
 * - Los JSON se leen y se arman con cJSON (componente espressif/cjson).
 *
 * MESH_ID y MESH_AP_PASS deben coincidir con el gateway.
 * Canal 0 = busca la malla en todos los canales (la raíz usa el canal del router).
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
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "cJSON.h"

/* ---------- Configuración común de la malla (igual que el gateway) ---------- */
#define MESH_MAX_HIJOS      6
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
#define DEVICE_ID           AULA "-concentrador"
#define TOPICO_ESTADO       "aura/" AULA "/concentrador/estado"
/* --------------------------------------------------------------------------------- */

/* Vendor IE de AURA: OUI propio (administrado localmente) + tipo 1 = "concentrador" */
#define AURA_OUI_0          0x02
#define AURA_OUI_1          0xA5
#define AURA_OUI_2          0x52
#define AURA_IE_CONCENTRADOR 0x01

#define PIN_BOTON           GPIO_NUM_0         /* botón BOOT de la placa */
#define PIN_LED_RGB         38                 /* WS2812 de la Waveshare ESP32-S3-DEV-KIT (en otras S3 es el 48) */
#define BRILLO_LED          150                /* 0-255: el azul a 60 casi no se ve */

static const char *TAG = "CONCENTRADOR";
static volatile bool conectado = false;
static volatile int hijos = 0;
static volatile int fallos_envio = 0;

#define LIMITE_ENVIO_MS     5000               /* tiempo máximo de un esp_mesh_send() */
#define FALLOS_PARA_RECONECTAR 3               /* envíos fallidos seguidos a la raíz -> reconexión */
static led_strip_handle_t led_rgb = NULL;

/* Hojas del aula: id -> MAC, aprendido de sus mensajes (no se configura ninguna MAC) */
#define MAX_HOJAS           8
typedef struct {
    char id[32];
    uint8_t mac[6];
} hoja_t;
static hoja_t hojas[MAX_HOJAS];
static int cant_hojas = 0;
static portMUX_TYPE hojas_lock = portMUX_INITIALIZER_UNLOCKED;
static int hoja_led = -1;                      /* -1 = todavía no informó */

/* ------------------------------------------------------------------ */
/*  "Pantalla": LED RGB + puerto serie                                 */
/* ------------------------------------------------------------------ */
static void led_mostrar(int estado)
{
    if (led_rgb == NULL) {
        return;
    }
    if (estado == 1) {
        led_strip_set_pixel(led_rgb, 0, 0, 0, BRILLO_LED);   /* azul, como el LED de la hoja */
        led_strip_refresh(led_rgb);
    } else {
        led_strip_clear(led_rgb);
    }
}

static void led_iniciar(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_LED_RGB,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,   /* este LED usa orden R-G-B */
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&cfg, &rmt, &led_rgb) != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo iniciar el LED RGB en GPIO%d", PIN_LED_RGB);
        led_rgb = NULL;
        return;
    }
    led_strip_clear(led_rgb);
}

/* ------------------------------------------------------------------ */
/*  Hora (la reparte el gateway con {"dest":"<id>","hora":<ms UTC>})  */
/* ------------------------------------------------------------------ */
static bool hora_valida(void)
{
    return time(NULL) > 1700000000;              /* posterior a 2023: ya la recibió */
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
        ESP_LOGI(TAG, "Hora recibida del gateway");
    }
}

/* ------------------------------------------------------------------ */
/*  JSON (cJSON)                                                       */
/* ------------------------------------------------------------------ */

/* Texto de un campo, o NULL si no está o no es texto */
static const char *json_txt(const cJSON *obj, const char *clave)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, clave);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* Valor numérico de un campo (true/false cuentan como 1/0). false si no está. */
static bool json_num(const cJSON *obj, const char *clave, double *valor)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, clave);
    if (cJSON_IsNumber(v)) {
        *valor = v->valuedouble;
    } else if (cJSON_IsBool(v)) {
        *valor = cJSON_IsTrue(v) ? 1 : 0;
    } else {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Mensajes de la malla                                               */
/* ------------------------------------------------------------------ */
static void enviar(const mesh_addr_t *destino, const char *msg)
{
    mesh_data_t data = {
        .data = (uint8_t *)msg,
        .size = (uint16_t)strlen(msg),
        .proto = MESH_PROTO_JSON,
        .tos = MESH_TOS_P2P,
    };
    esp_err_t err = esp_mesh_send(destino, &data, MESH_DATA_P2P, NULL, 0);
    if (destino != NULL) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Error al enviar a una hoja: %s", esp_err_to_name(err));
        }
        return;
    }
    /* Vigilante de envío hacia la raíz: si la raíz se reinició, el control de flujo de
       la malla puede quedar trabado ([WND-RX] timeout) aunque el concentrador figure
       conectado. Tras varios fallos seguidos se fuerza la reconexión. */
    if (err == ESP_OK) {
        fallos_envio = 0;
        return;
    }
    ESP_LOGW(TAG, "Error al enviar a la raíz: %s", esp_err_to_name(err));
    if (conectado && ++fallos_envio >= FALLOS_PARA_RECONECTAR) {
        ESP_LOGW(TAG, "%d envíos fallidos seguidos: fuerzo la reconexión", fallos_envio);
        fallos_envio = 0;
        esp_mesh_disconnect();
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_mesh_connect();
    }
}

/* Convierte el objeto a texto y lo envía (no libera el objeto) */
static void enviar_json(const mesh_addr_t *destino, const cJSON *json)
{
    char *txt = cJSON_PrintUnformatted(json);
    if (txt != NULL) {
        enviar(destino, txt);
        cJSON_free(txt);
    }
}

static void hoja_aprender(const char *id, const uint8_t mac[6])
{
    bool nueva = false, cambio = false;
    taskENTER_CRITICAL(&hojas_lock);
    int i;
    for (i = 0; i < cant_hojas && strcmp(hojas[i].id, id) != 0; i++) {
    }
    if (i == cant_hojas && cant_hojas < MAX_HOJAS) {
        strlcpy(hojas[i].id, id, sizeof(hojas[i].id));
        memcpy(hojas[i].mac, mac, 6);
        cant_hojas++;
        nueva = true;
    } else if (i < cant_hojas && memcmp(hojas[i].mac, mac, 6) != 0) {
        memcpy(hojas[i].mac, mac, 6);
        cambio = true;
    }
    taskEXIT_CRITICAL(&hojas_lock);
    if (nueva || cambio) {
        ESP_LOGI(TAG, "Hoja del aula %s: %s = " MACSTR, nueva ? "registrada" : "actualizada",
                 id, MAC2STR(mac));
    }
}

static bool hoja_buscar(const char *id, mesh_addr_t *mac)
{
    bool ok = false;
    taskENTER_CRITICAL(&hojas_lock);
    for (int i = 0; i < cant_hojas; i++) {
        if (id == NULL || strcmp(hojas[i].id, id) == 0) {   /* NULL = la primera */
            memcpy(mac->addr, hojas[i].mac, 6);
            ok = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&hojas_lock);
    return ok;
}

/* Muestra en la "pantalla" lo que manda una hoja (sensores y/o estado del LED) */
static void mostrar_en_pantalla(const cJSON *json)
{
    double v;

    if (json_num(json, "temp", &v)) {
        double hum = 0, pres = 0;
        json_num(json, "hum", &hum);
        json_num(json, "pres", &pres);
        printf("PANTALLA " AULA " | %.1f °C  %.1f %%  presencia: %s\n",
               v, hum, pres ? "sí" : "no");
    }
    if (json_num(json, "led", &v)) {
        int nuevo = (int)v;
        const char *origen = json_txt(json, "origen");     /* solo viene en las respuestas */
        if (origen != NULL || nuevo != hoja_led) {
            printf("PANTALLA " AULA " | LED de la hoja: %s (%s%s)\n",
                   nuevo ? "ENCENDIDO" : "apagado",
                   origen ? "orden " : "estado periódico", origen ? origen : "");
        }
        if (nuevo != hoja_led) {
            led_mostrar(nuevo);
        }
        hoja_led = nuevo;
    }
}

/*
 * Todo lo que llega dirigido al concentrador:
 *  - con "dest" = este concentrador: mensaje para él (p. ej. la hora)
 *  - con "dest" = una hoja: baja de AURA -> se le pasa a esa hoja (órdenes, hora)
 *  - sin "dest": mensaje de una hoja del aula -> pantalla + reenvío a la raíz
 */
static void tarea_recepcion(void *arg)
{
    static uint8_t buf[600];
    mesh_addr_t origen, destino;
    mesh_data_t data;
    int flag = 0;
    double v;

    while (true) {
        data.data = buf;
        data.size = sizeof(buf) - 1;
        if (esp_mesh_recv(&origen, &data, portMAX_DELAY, &flag, NULL, 0) != ESP_OK) {
            continue;
        }
        buf[data.size] = '\0';

        cJSON *json = cJSON_Parse((const char *)buf);
        if (!cJSON_IsObject(json)) {
            ESP_LOGW(TAG, "Mensaje que no es JSON de " MACSTR ": %s", MAC2STR(origen.addr), (char *)buf);
            cJSON_Delete(json);
            continue;
        }

        const char *dest = json_txt(json, "dest");
        if (dest != NULL) {
            if (strcmp(dest, DEVICE_ID) == 0) {
                if (json_num(json, "hora", &v)) {
                    hora_ajustar(v);
                } else {
                    ESP_LOGI(TAG, "Orden para el concentrador (sin uso por ahora): %s", (char *)buf);
                }
            } else if (hoja_buscar(dest, &destino)) {
                if (!cJSON_HasObjectItem(json, "hora")) {
                    ESP_LOGI(TAG, "Orden de AURA -> %s: %s", dest, (char *)buf);
                }
                enviar(&destino, (char *)buf);   /* se pasa tal cual */
            } else {
                ESP_LOGW(TAG, "Mensaje para '%s', que no es una hoja de este concentrador", dest);
            }
            cJSON_Delete(json);
            continue;
        }

        const char *id = json_txt(json, "id");
        if (id != NULL && strncmp(id, AULA "-", strlen(AULA) + 1) == 0 && strlen(id) < sizeof(hojas[0].id)) {
            hoja_aprender(id, origen.addr);
        }
        mostrar_en_pantalla(json);
        enviar(NULL, (char *)buf);             /* reenvío a la raíz, sin cambios */
        cJSON_Delete(json);
    }
}

/* Mando local: un toque en BOOT invierte el LED de la hoja */
static void tarea_boton(void *arg)
{
    mesh_addr_t hoja;
    int anterior = 1;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30));
        int nivel = gpio_get_level(PIN_BOTON);
        if (anterior == 1 && nivel == 0) {             /* flanco de bajada = toque */
            vTaskDelay(pdMS_TO_TICKS(30));             /* antirrebote */
            if (gpio_get_level(PIN_BOTON) == 0) {
                if (conectado && hoja_buscar(NULL, &hoja)) {
                    ESP_LOGI(TAG, "Botón: orden local a la hoja");
                    cJSON *orden = cJSON_CreateObject();
                    cJSON_AddStringToObject(orden, "led", "toggle");
                    cJSON_AddStringToObject(orden, "origen", "local");
                    enviar_json(&hoja, orden);
                    cJSON_Delete(orden);
                } else {
                    ESP_LOGW(TAG, "Botón: todavía no hay hoja conectada");
                }
            }
        }
        anterior = nivel;
    }
}

static void tarea_estado(void *arg)
{
    uint32_t contador = 0;
    uint8_t mac[6];
    char mac_txt[18];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_txt, sizeof(mac_txt), MACSTR, MAC2STR(mac));

    while (true) {
        if (conectado) {
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "topic", TOPICO_ESTADO);
            cJSON_AddStringToObject(msg, "id", DEVICE_ID);
            cJSON_AddStringToObject(msg, "mac", mac_txt);
            if (hora_valida()) {
                cJSON_AddNumberToObject(msg, "ts", (double)ahora_ms());
            }
            cJSON_AddNumberToObject(msg, "n", contador++);
            cJSON_AddNumberToObject(msg, "capa", esp_mesh_get_layer());
            cJSON_AddNumberToObject(msg, "hijos", hijos);
            cJSON_AddNumberToObject(msg, "hojas_aula", cant_hojas);
            cJSON_AddNumberToObject(msg, "nodos", esp_mesh_get_total_node_num());
            cJSON_AddNumberToObject(msg, "led_hoja", hoja_led);
            enviar_json(NULL, msg);
            cJSON_Delete(msg);
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* ------------------------------------------------------------------ */
/*  Anuncio del aula en el beacon (vendor IE)                          */
/* ------------------------------------------------------------------ */
static void anunciar_aula(void)
{
    static uint8_t ie[sizeof(vendor_ie_data_t) + 6 + sizeof(AULA)];
    vendor_ie_data_t *v = (vendor_ie_data_t *)ie;
    uint8_t mac_sta[6];

    esp_read_mac(mac_sta, ESP_MAC_WIFI_STA);   /* la dirección con la que se le habla en la malla */
    v->element_id = WIFI_VENDOR_IE_ELEMENT_ID;
    v->vendor_oui[0] = AURA_OUI_0;
    v->vendor_oui[1] = AURA_OUI_1;
    v->vendor_oui[2] = AURA_OUI_2;
    v->vendor_oui_type = AURA_IE_CONCENTRADOR;
    memcpy(v->payload, mac_sta, 6);
    memcpy(v->payload + 6, AULA, strlen(AULA));
    v->length = 4 + 6 + strlen(AULA);

    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_1, NULL);
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_1, NULL);
    esp_err_t e1 = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_1, v);
    esp_err_t e2 = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_PROBE_RESP, WIFI_VND_IE_ID_1, v);
    if (e1 == ESP_OK && e2 == ESP_OK) {
        ESP_LOGI(TAG, "Anunciando aula '%s' en el beacon", AULA);
    } else {
        ESP_LOGW(TAG, "No se pudo anunciar el aula: %s / %s", esp_err_to_name(e1), esp_err_to_name(e2));
    }
}

static void manejador_eventos_mesh(void *arg, esp_event_base_t base,
                                   int32_t id, void *event_data)
{
    switch (id) {
    case MESH_EVENT_STARTED:
        anunciar_aula();
        break;
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *c = (mesh_event_connected_t *)event_data;
        conectado = true;
        ESP_LOGI(TAG, "Conectado al padre " MACSTR " - capa %d, canal %d",
                 MAC2STR(c->connected.bssid), esp_mesh_get_layer(), c->connected.channel);
        anunciar_aula();                       /* el AP puede haberse reiniciado al cambiar de canal */
        break;
    }
    case MESH_EVENT_PARENT_DISCONNECTED:
        conectado = false;
        ESP_LOGW(TAG, "Desconectado del padre, buscando...");
        break;
    case MESH_EVENT_NO_PARENT_FOUND:
        ESP_LOGW(TAG, "No se encontró padre (¿gateway encendido? ¿mismo MESH_ID?)");
        break;
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *c = (mesh_event_child_connected_t *)event_data;
        hijos++;
        ESP_LOGI(TAG, "Hijo conectado: " MACSTR " (aid %d)", MAC2STR(c->mac), c->aid);
        break;
    }
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *c = (mesh_event_child_disconnected_t *)event_data;
        if (hijos > 0) {
            hijos--;
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

    led_iniciar();
    gpio_config_t boton = {
        .pin_bit_mask = 1ULL << PIN_BOTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boton);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif_sta = NULL;
    esp_netif_t *netif_ap = NULL;
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, &netif_ap));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                               &manejador_eventos_mesh, NULL));

    ESP_ERROR_CHECK(esp_mesh_fix_root(true));          /* la raíz es fija: no compite */
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_CAPAS));
    ESP_ERROR_CHECK(esp_mesh_disable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK));

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&cfg.mesh_id, MESH_ID, 6);
    cfg.channel = 0;                                   /* buscar en todos los canales */
    cfg.allow_channel_switch = true;

    cfg.router.ssid_len = strlen(ROUTER_FICTICIO);
    memcpy(cfg.router.ssid, ROUTER_FICTICIO, cfg.router.ssid_len);

    cfg.mesh_ap.max_connection = MESH_MAX_HIJOS;
    cfg.mesh_ap.nonmesh_max_connection = 0;
    memcpy(cfg.mesh_ap.password, MESH_AP_PASS, strlen(MESH_AP_PASS));

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_send_block_time(LIMITE_ENVIO_MS));   /* que ningún envío se trabe para siempre */
    ESP_ERROR_CHECK(esp_mesh_start());

    xTaskCreate(tarea_recepcion, "mesh_rx", 4096, NULL, 5, NULL);
    xTaskCreate(tarea_estado, "mesh_tx", 4096, NULL, 5, NULL);
    xTaskCreate(tarea_boton, "boton", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Concentrador de %s iniciado, buscando al gateway...", AULA);
}
