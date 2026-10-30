#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_mesh.h"
#include "esp_event.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include "aura_ota.h"
#include "aura_fw.h"
#include "aura_ota_receptor.h"

static const char *TAG = "AURA_OTA";

/* Dirección de la raíz de la malla (MESH_EVENT_ROOT_ADDRESS). Solo la raíz ofrece y manda
   actualizaciones: una trama OTA de otro nodo se descarta. */
static portMUX_TYPE lock_raiz = portMUX_INITIALIZER_UNLOCKED;
static uint8_t raiz[6];
static bool hay_raiz = false;

typedef struct {
    uint16_t largo;
    uint8_t datos[AURA_OTA_TRAMA_MAX];
} trama_ota_t;

static aura_ota_receptor_cfg_t cfg;
static QueueHandle_t cola = NULL;

/* Sesión en curso (solo la toca la tarea ota) */
static volatile bool activa = false;
static uint32_t sesion = 0;
static aura_ota_oferta_t oferta;
static const esp_partition_t *destino = NULL;
static esp_ota_handle_t manejador = 0;
static psa_hash_operation_t hash;
static uint32_t recibido = 0;                  /* bytes escritos: el próximo que se espera */
static uint32_t pedido_desde = 0;              /* desde dónde fue el último pedido */
static TickType_t ultimo_dato = 0;
static int pedidos_sin_respuesta = 0;
static int ultimo_porcentaje = -1;

/* A la raíz: no pasa por la lógica del concentrador */
static bool enviar(uint8_t op, uint32_t s, const void *cuerpo, size_t largo)
{
    uint8_t buf[AURA_OTA_CABECERA + 4 + AURA_OTA_MOTIVO_MAX + 1];
    if (largo > sizeof(buf) - AURA_OTA_CABECERA) {
        largo = sizeof(buf) - AURA_OTA_CABECERA;
    }
    memcpy(aura_ota_cabecera(buf, op, s), cuerpo, largo);
    mesh_data_t d = {.data = buf, .size = AURA_OTA_CABECERA + largo, .proto = MESH_PROTO_BIN, .tos = MESH_TOS_P2P};
    esp_err_t err = esp_mesh_send(NULL, &d, MESH_DATA_P2P, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo avisar a la raíz: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static void pedir(uint32_t desde)
{
    uint8_t p[4];
    aura_u32_escribir(p, desde);
    pedido_desde = desde;
    ultimo_dato = xTaskGetTickCount();
    enviar(AURA_OTA_PIDO, sesion, p, sizeof(p));
}

static void fin(uint32_t s, uint8_t resultado, const char *motivo)
{
    uint8_t p[1 + AURA_OTA_MOTIVO_MAX];
    size_t n = motivo ? strnlen(motivo, AURA_OTA_MOTIVO_MAX) : 0;
    p[0] = resultado;
    if (n) {
        memcpy(p + 1, motivo, n);
    }
    enviar(AURA_OTA_FIN, s, p, 1 + n);
}

static void cerrar(void)
{
    if (activa) {
        esp_ota_abort(manejador);
        psa_hash_abort(&hash);
    }
    activa = false;
}

static void abandonar(const char *motivo)
{
    ESP_LOGW(TAG, "Actualización abandonada: %s (%lu de %lu bytes)", motivo, (unsigned long)recibido,
             (unsigned long)oferta.tamano);
    uint32_t s = sesion;
    cerrar();
    fin(s, AURA_OTA_RECHAZO, motivo);
}

static void atender_oferta(uint32_t s, const uint8_t *cuerpo, size_t largo)
{
    if (largo < sizeof(aura_ota_oferta_t)) {
        return;
    }
    if (activa && s == sesion) {
        pedir(recibido);                               /* la oferta repetida: el pedido se perdió */
        return;
    }
    if (activa) {
        ESP_LOGW(TAG, "Oferta nueva: abandono la anterior");
        cerrar();
    }
    aura_ota_oferta_t o;
    memcpy(&o, cuerpo, sizeof(o));
    o.tipo[AURA_OTA_TEXTO - 1] = '\0';
    o.version[AURA_OTA_TEXTO - 1] = '\0';
    const char *motivo = NULL;
    if (o.chip != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        motivo = "chip_distinto";
    } else if (o.tamano < 1024) {
        motivo = "imagen_invalida";
    } else if (!aura_tipo_valido(o.tipo) || !aura_version_valida(o.version)) {
        motivo = "imagen_sin_tipo_o_version";
    } else if (strcmp(o.tipo, aura_fw_tipo()) != 0) {
        /* Sin excepciones: cambiarle el uso a una placa se hace por USB (o.forzar se ignora) */
        motivo = "tipo_distinto";
    } else if (strcmp(o.tipo, aura_fw_tipo()) == 0 && strcmp(o.version, aura_fw_version()) == 0) {
        ESP_LOGI(TAG, "Oferta de %s %s: ya es la que corre", o.tipo, o.version);
        fin(s, AURA_OTA_YA_TIENE, NULL);
        return;
    } else if (aura_fw_sin_confirmar()) {
        motivo = "version_actual_sin_confirmar";      /* pisaría la única versión que se sabe que anda */
    }
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (motivo == NULL && p == NULL) {
        motivo = "sin_particion_ota";
    } else if (motivo == NULL && o.tamano > p->size) {
        motivo = "imagen_muy_grande";
    }
    if (motivo == NULL && esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &manejador) != ESP_OK) {
        motivo = "error_de_flash";
    }
    if (motivo) {
        ESP_LOGW(TAG, "Oferta de %s %s rechazada: %s", o.tipo, o.version, motivo);
        fin(s, AURA_OTA_RECHAZO, motivo);
        return;
    }
    hash = psa_hash_operation_init();
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        esp_ota_abort(manejador);
        fin(s, AURA_OTA_RECHAZO, "error_de_sha256");
        return;
    }
    oferta = o;
    sesion = s;
    destino = p;
    recibido = 0;
    pedidos_sin_respuesta = 0;
    ultimo_porcentaje = -1;
    activa = true;
    ESP_LOGI(TAG, "Actualización a %s %s (%lu bytes) en %s: empiezo a pedir", o.tipo, o.version,
             (unsigned long)o.tamano, p->label);
    pedir(0);
}

/* La imagen tiene que decir lo mismo que la oferta (tipo, versión y chip) */
static bool encabezado_coincide(const uint8_t *d, size_t n)
{
    size_t pos = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (n < pos + sizeof(esp_app_desc_t)) {
        return false;
    }
    const esp_image_header_t *h = (const esp_image_header_t *)d;
    esp_app_desc_t desc;
    memcpy(&desc, d + pos, sizeof(desc));
    return h->magic == ESP_IMAGE_HEADER_MAGIC && h->chip_id == oferta.chip &&
           desc.magic_word == ESP_APP_DESC_MAGIC_WORD &&
           strncmp(desc.project_name, oferta.tipo, sizeof(desc.project_name)) == 0 &&
           strncmp(desc.version, oferta.version, sizeof(desc.version)) == 0;
}

static void terminar(void)
{
    uint8_t sha[32];
    size_t n = 0;
    if (psa_hash_finish(&hash, sha, sizeof(sha), &n) != PSA_SUCCESS || n != 32 ||
        memcmp(sha, oferta.sha256, 32) != 0) {
        psa_hash_abort(&hash);
        esp_ota_abort(manejador);
        activa = false;
        ESP_LOGW(TAG, "El SHA-256 de lo recibido no coincide: no se instala");
        fin(sesion, AURA_OTA_RECHAZO, "sha256_distinto");
        return;
    }
    activa = false;
    esp_err_t err = esp_ota_end(manejador);           /* valida la imagen (formato y su propio hash) */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Imagen inválida: %s", esp_err_to_name(err));
        fin(sesion, AURA_OTA_RECHAZO, "imagen_invalida");
        return;
    }
    if (esp_ota_set_boot_partition(destino) != ESP_OK) {
        fin(sesion, AURA_OTA_RECHAZO, "error_de_flash");
        return;
    }
    ESP_LOGW(TAG, "Imagen %s %s verificada: me reinicio con ella", oferta.tipo, oferta.version);
    for (int i = 0; i < 3; i++) {                      /* el FIN puede perderse: se repite */
        fin(sesion, AURA_OTA_LISTO, NULL);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (cfg.antes_de_reiniciar) {
        cfg.antes_de_reiniciar();
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void atender_datos(uint32_t s, const uint8_t *cuerpo, size_t largo)
{
    if (largo < 4) {
        return;
    }
    if (!activa || s != sesion) {
        /* Por ejemplo, el nodo se reinició a mitad: que el gateway lo sepa ya y no espere */
        static uint32_t avisada = 0;
        if (s != avisada) {
            avisada = s;
            fin(s, AURA_OTA_RECHAZO, "sesion_desconocida");
        }
        return;
    }
    uint32_t desde = aura_u32_leer(cuerpo);
    const uint8_t *bytes = cuerpo + 4;
    size_t n = largo - 4;
    if (desde != recibido || n == 0 || recibido + n > oferta.tamano) {
        return;                                        /* fuera de orden: se vuelve a pedir */
    }
    if (desde == 0 && !encabezado_coincide(bytes, n)) {
        abandonar("imagen_distinta_de_la_oferta");
        return;
    }
    if (esp_ota_write(manejador, bytes, n) != ESP_OK) {
        abandonar("error_de_flash");
        return;
    }
    psa_hash_update(&hash, bytes, n);
    recibido += n;
    ultimo_dato = xTaskGetTickCount();
    pedidos_sin_respuesta = 0;
    int porcentaje = (int)((uint64_t)recibido * 100 / oferta.tamano);
    if (porcentaje / 10 != ultimo_porcentaje / 10) {
        ultimo_porcentaje = porcentaje;
        ESP_LOGI(TAG, "Recibido %d %%", porcentaje);
    }
    if (recibido == oferta.tamano) {
        terminar();
    } else if (recibido >= pedido_desde + AURA_OTA_VENTANA * AURA_OTA_BLOQUE) {
        pedir(recibido);
    }
}

static void tarea_ota(void *arg)
{
    static trama_ota_t t;
    while (true) {
        if (xQueueReceive(cola, &t, pdMS_TO_TICKS(500)) == pdTRUE) {
            uint32_t s = aura_ota_sesion(t.datos);
            const uint8_t *cuerpo = t.datos + AURA_OTA_CABECERA;
            size_t largo = t.largo - AURA_OTA_CABECERA;
            switch (aura_ota_op(t.datos)) {
            case AURA_OTA_OFERTA:
                atender_oferta(s, cuerpo, largo);
                break;
            case AURA_OTA_DATOS:
                atender_datos(s, cuerpo, largo);
                break;
            case AURA_OTA_CONFIRMADA:
                aura_fw_confirmar();                   /* el gateway la vio volver: la versión anda */
                break;
            case AURA_OTA_CANCELAR:
                if (activa && s == sesion) {
                    ESP_LOGW(TAG, "El gateway canceló la actualización");
                    cerrar();
                }
                break;
            default:
                break;
            }
        }
        /* Sin datos: se vuelve a pedir; si el gateway no contesta más, se abandona */
        if (activa && xTaskGetTickCount() - ultimo_dato >= pdMS_TO_TICKS(AURA_OTA_ESPERA_MS)) {
            if (++pedidos_sin_respuesta > AURA_OTA_PEDIDOS_MAX) {
                abandonar("sin_datos_del_gateway");
            } else {
                ESP_LOGW(TAG, "Sin datos hace %d s: vuelvo a pedir desde %lu", AURA_OTA_ESPERA_MS / 1000,
                         (unsigned long)recibido);
                pedir(recibido);
            }
        }
    }
}

static void al_saber_la_raiz(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    mesh_event_root_address_t *r = (mesh_event_root_address_t *)data;
    /* La pila informa la MAC del AP de la raíz; sus tramas salen de su MAC de estación, que es una
       menos (AP = estación + 1 en los ESP32). Se resta 1 a los 48 bits. */
    uint8_t sta[6];
    memcpy(sta, r->addr, 6);
    for (int i = 5; i >= 0 && sta[i]-- == 0; i--) {
    }
    taskENTER_CRITICAL(&lock_raiz);
    memcpy(raiz, sta, 6);
    hay_raiz = true;
    taskEXIT_CRITICAL(&lock_raiz);
}

bool aura_es_la_raiz(const uint8_t mac[6])
{
    taskENTER_CRITICAL(&lock_raiz);
    bool es = hay_raiz && memcmp(raiz, mac, 6) == 0;
    taskEXIT_CRITICAL(&lock_raiz);
    return es;
}

void aura_ota_receptor_iniciar(const aura_ota_receptor_cfg_t *c)
{
    if (c) {
        cfg = *c;
    }
    esp_event_handler_register(MESH_EVENT, MESH_EVENT_ROOT_ADDRESS, al_saber_la_raiz, NULL);
    psa_crypto_init();
    cola = xQueueCreate(6, sizeof(trama_ota_t));
    xTaskCreate(tarea_ota, "ota", 4096, NULL, 4, NULL);
}

void aura_ota_recibir(const uint8_t de[6], const uint8_t *datos, size_t largo)
{
    if (cola == NULL || !aura_es_ota(datos, largo)) {
        return;
    }
    if (!aura_es_la_raiz(de)) {
        ESP_LOGW(TAG, "Trama OTA de " MACSTR ", que no es la raíz: se descarta", MAC2STR(de));
        return;
    }
    static trama_ota_t t;                              /* solo la llama la tarea de recepción */
    t.largo = (uint16_t)largo;
    memcpy(t.datos, datos, largo);
    if (xQueueSend(cola, &t, 0) != pdTRUE) {
        ESP_LOGD(TAG, "Cola OTA llena: se pierde una trama (se vuelve a pedir)");
    }
}

bool aura_ota_en_curso(void)
{
    return activa;
}
