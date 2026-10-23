/*
 * Actualización por la malla, lado gateway.
 *
 * - Cada comando "ota" entra a una cola, por placa (hw_id). Se atiende UNA placa por vez: la malla
 *   no se satura y un aula nunca se queda sin todos sus dispositivos a la vez.
 * - La imagen se descarga UNA vez por HTTP(S) a la partición "imagen" y se verifica su SHA-256
 *   contra el del comando. Si varias placas piden la misma imagen, se usa la descargada.
 * - Antes de mandar nada se compara con lo que la placa informó en su reporte (fw): si es de otro
 *   tipo se rechaza (salvo forzar_tipo) y si ya tiene esa versión se contesta aplicado sin_cambios.
 * - Transferencia: oferta -> el nodo pide desde un byte -> se le manda una ventana de bloques. Si el
 *   nodo deja de pedir (se reinició, se cayó su padre), se reintenta desde cero hasta REINTENTOS_MAX
 *   veces. Al terminar, el nodo verifica y se reinicia; la actualización se da por aplicada recién
 *   cuando la placa vuelve y reporta la versión nueva.
 * - El propio gateway se actualiza último (cuando no queda nada más), copiando la imagen a su otra
 *   partición. Lo que estaba en la cola de un gateway que se reinicia se pierde: quien pidió la
 *   actualización (ota.py o AURA) lo vuelve a pedir; es idempotente (sin_cambios).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_mesh.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include "aura_ota.h"
#include "aura_util.h"
#include "aura_fw.h"
#include "gateway.h"
#include "ota_gateway.h"

static const char *TAG = "OTA_GW";

#define TRABAJOS_CAP            32
#define URL_MAX                 160
#define REINTENTOS_MAX          2       /* transferencias cortadas que se vuelven a empezar */
#define REPETIR_OFERTA_MS       3000
#define OFERTAS_MAX             10      /* ~30 s sin respuesta a la oferta -> el nodo no responde */
#define ALCANZAR_MS             120000  /* la malla no acepta el envío tanto tiempo -> no alcanzable */
#define ESPERA_VUELTA_MS        (15 * 60 * 1000)   /* > AURA_FW_VERIFICAR_S: ve también la vuelta atrás */
#define GUARDA_VUELTA_MS        3000    /* un reporte viejo que ya venía en camino no cuenta */
#define GRACIA_PROPIO_MS        15000   /* el gateway espera tanto en la cola: que entre toda la tanda */
#define SONDEO_MS               8000    /* el nodo no pide nada tanto: se le reenvía la última ventana */
#define SUBTIPO_IMAGEN          0x41
#define NVS_OTA                 "gw_ota"

typedef enum { LIBRE = 0, EN_COLA, EN_CURSO, ESPERANDO_VUELTA } estado_t;

typedef struct {
    estado_t estado;
    uint8_t mac[6];
    char command_id[48];
    char url[URL_MAX];
    uint8_t sha[32];
    bool forzar;
    int reintentos;
    uint32_t desde_ms;                  /* cuándo entró a la cola */
    uint32_t listo_ms;                  /* cuándo avisó que tenía la imagen (ESPERANDO_VUELTA) */
    uint32_t volvio_ms;                 /* cuándo volvió con la versión nueva; 0 = todavía no */
    char tipo[AURA_OTA_TEXTO];          /* lo que se le instaló: con eso tiene que volver */
    char version[AURA_OTA_TEXTO];
} trabajo_t;

typedef struct {
    uint8_t mac[6];
    uint8_t op;
    uint32_t sesion;
    uint32_t desde;
    uint8_t resultado;
    char motivo[AURA_OTA_MOTIVO_MAX + 1];
} aviso_t;

/* El gateway se actualizó a sí mismo: para contestar después del reinicio */
typedef struct {
    char command_id[48];
    char version[AURA_OTA_TEXTO];
} propio_t;

static trabajo_t trabajos[TRABAJOS_CAP];
static SemaphoreHandle_t lock = NULL;
static QueueHandle_t avisos = NULL;
static const esp_partition_t *part_imagen = NULL;

/* La imagen que está en la partición "imagen" */
static bool img_ok = false;
static uint8_t img_sha[32];
static uint32_t img_tamano = 0;
static uint16_t img_chip = 0;
static char img_tipo[AURA_OTA_TEXTO], img_version[AURA_OTA_TEXTO];

/* Para el status */
static volatile int progreso = -1;
static uint8_t mac_actual[6];

static uint8_t trama[AURA_OTA_TRAMA_MAX];      /* solo la usa la tarea ota */

static uint32_t ahora_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool hex_a_bytes(const char *h, uint8_t *b, size_t n)
{
    if (h == NULL || strlen(h) != n * 2) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int a = aura_hex(h[2 * i]), c = aura_hex(h[2 * i + 1]);
        if (a < 0 || c < 0) {
            return false;
        }
        b[i] = (uint8_t)(a << 4 | c);
    }
    return true;
}

/* details.firmware = {"tipo":..., "version":...} */
static cJSON *firmware(const char *tipo, const char *version, bool sin_cambios)
{
    cJSON *x = cJSON_CreateObject();
    cJSON *f = cJSON_AddObjectToObject(x, "firmware");
    cJSON_AddStringToObject(f, "tipo", tipo);
    cJSON_AddStringToObject(f, "version", version);
    if (sin_cambios) {
        cJSON_AddBoolToObject(x, "sin_cambios", true);
    }
    return x;
}

static void responder_fw(const trabajo_t *t, const char *status, const char *tipo, const char *version,
                         bool sin_cambios)
{
    cJSON *x = firmware(tipo, version, sin_cambios);
    gw_responder(t->mac, status, t->command_id, NULL, x);
    cJSON_Delete(x);
}

/* "tipo@versión" -> partes. false si no tiene ese formato */
static bool partir_fw(const char *fw, char tipo[AURA_OTA_TEXTO], char version[AURA_OTA_TEXTO])
{
    const char *arroba = strchr(fw, '@');
    if (arroba == NULL || arroba == fw || (size_t)(arroba - fw) >= AURA_OTA_TEXTO ||
        strlen(arroba + 1) >= AURA_OTA_TEXTO) {
        return false;
    }
    memcpy(tipo, fw, arroba - fw);
    tipo[arroba - fw] = '\0';
    strcpy(version, arroba + 1);
    return true;
}

static void terminar(int i)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    trabajos[i].estado = LIBRE;
    xSemaphoreGive(lock);
}

static void rechazar(int i, const char *motivo)
{
    trabajo_t t = trabajos[i];
    char hw[AURA_HW_ID_BYTES];
    aura_hw_id_de_mac(t.mac, hw);
    ESP_LOGW(TAG, "%s: rechazada (%s)", hw, motivo);
    gw_responder(t.mac, "rechazado", t.command_id, motivo, NULL);
    terminar(i);
}

/* ================================================================== */
/*  Pedidos (MQTT) y avisos de los nodos                               */
/* ================================================================== */
void ota_gw_pedido(const uint8_t mac[6], const char *json)
{
    cJSON *d = cJSON_Parse(json);
    const cJSON *ci = cJSON_GetObjectItemCaseSensitive(d, "command_id");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(d, "params");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(p, "url");
    const cJSON *sha = cJSON_GetObjectItemCaseSensitive(p, "sha256");
    const cJSON *forzar = cJSON_GetObjectItemCaseSensitive(p, "forzar_tipo");
    trabajo_t t = {.estado = EN_COLA, .desde_ms = ahora_ms()};
    memcpy(t.mac, mac, 6);
    if (cJSON_IsString(ci)) {
        strlcpy(t.command_id, ci->valuestring, sizeof(t.command_id));
    }
    const char *motivo = NULL;
    if (!cJSON_IsString(url) || strlen(url->valuestring) >= URL_MAX ||
        (strncmp(url->valuestring, "http://", 7) != 0 && strncmp(url->valuestring, "https://", 8) != 0)) {
        motivo = "url_invalida";
    } else if (!cJSON_IsString(sha) || !hex_a_bytes(sha->valuestring, t.sha, 32)) {
        motivo = "sha256_invalido";
    } else if (forzar != NULL && !cJSON_IsBool(forzar)) {
        motivo = "params_invalidos";
    } else {
        strlcpy(t.url, url->valuestring, sizeof(t.url));
        t.forzar = cJSON_IsTrue(forzar);
    }
    cJSON_Delete(d);
    if (motivo == NULL && part_imagen == NULL) {
        motivo = "gateway_sin_particion_imagen";
    }
    if (motivo) {
        gw_responder(mac, "rechazado", t.command_id, motivo, NULL);
        return;
    }
    char hw[AURA_HW_ID_BYTES];
    aura_hw_id_de_mac(mac, hw);
    int libre = -1, en_cola = -1;
    bool en_curso = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < TRABAJOS_CAP; i++) {
        if (trabajos[i].estado == LIBRE) {
            if (libre < 0) {
                libre = i;
            }
        } else if (memcmp(trabajos[i].mac, mac, 6) == 0) {
            if (trabajos[i].estado == EN_COLA) {
                en_cola = i;
            } else if (trabajos[i].estado == EN_CURSO) {
                en_curso = true;
            }
        }
    }
    trabajo_t reemplazado = {0};
    if (!en_curso && en_cola >= 0) {               /* el pedido nuevo reemplaza al que esperaba */
        reemplazado = trabajos[en_cola];
        trabajos[en_cola] = t;
    } else if (!en_curso && libre >= 0) {
        trabajos[libre] = t;
    }
    xSemaphoreGive(lock);
    if (en_curso) {
        gw_responder(mac, "rechazado", t.command_id, "actualizacion_en_curso", NULL);
    } else if (en_cola >= 0) {
        ESP_LOGI(TAG, "%s: el pedido nuevo reemplaza al que estaba en la cola", hw);
        gw_responder(mac, "rechazado", reemplazado.command_id, "reemplazado_por_otro_pedido", NULL);
    } else if (libre < 0) {
        gw_responder(mac, "rechazado", t.command_id, "cola_ota_llena", NULL);
    } else {
        ESP_LOGI(TAG, "%s: actualización en la cola (%s)", hw, t.url);
    }
}

void ota_gw_trama(const uint8_t mac[6], const uint8_t *d, size_t n)
{
    if (avisos == NULL || !aura_es_ota(d, n)) {
        return;
    }
    aviso_t a = {.op = aura_ota_op(d), .sesion = aura_ota_sesion(d)};
    memcpy(a.mac, mac, 6);
    const uint8_t *cuerpo = d + AURA_OTA_CABECERA;
    size_t largo = n - AURA_OTA_CABECERA;
    if (a.op == AURA_OTA_PIDO && largo >= 4) {
        a.desde = aura_u32_leer(cuerpo);
    } else if (a.op == AURA_OTA_FIN && largo >= 1) {
        a.resultado = cuerpo[0];
        size_t m = largo - 1 < AURA_OTA_MOTIVO_MAX ? largo - 1 : AURA_OTA_MOTIVO_MAX;
        memcpy(a.motivo, cuerpo + 1, m);
        a.motivo[m] = '\0';
    } else {
        return;
    }
    xQueueSend(avisos, &a, 0);
}

void ota_gw_reporte(const uint8_t mac[6], const char *fw)
{
    char tipo[AURA_OTA_TEXTO], version[AURA_OTA_TEXTO];
    if (fw == NULL || !partir_fw(fw, tipo, version)) {
        return;
    }
    int k = -1;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < TRABAJOS_CAP && k < 0; i++) {
        if (trabajos[i].estado == ESPERANDO_VUELTA && memcmp(trabajos[i].mac, mac, 6) == 0 &&
            ahora_ms() - trabajos[i].listo_ms >= GUARDA_VUELTA_MS) {
            k = i;
        }
    }
    xSemaphoreGive(lock);
    if (k < 0) {
        return;
    }
    char hw[AURA_HW_ID_BYTES];
    aura_hw_id_de_mac(mac, hw);
    trabajo_t t = trabajos[k];
    if (strcmp(version, t.version) == 0 && strcmp(tipo, t.tipo) == 0) {
        if (t.volvio_ms) {
            return;                                    /* un reporte más: ya se sabía */
        }
        ESP_LOGI(TAG, "%s volvió con %s %s: si anda %d s sin colgarse, queda", hw, tipo, version, AURA_FW_ESTABLE_S);
        xSemaphoreTake(lock, portMAX_DELAY);
        trabajos[k].volvio_ms = ahora_ms() | 1;
        xSemaphoreGive(lock);
        /* Que sepa que habló con la raíz (cuenta para confirmar la versión), haya broker o no */
        uint8_t c[AURA_OTA_CABECERA];
        aura_ota_cabecera(c, AURA_OTA_CONFIRMADA, 0);
        mesh_addr_t destino;
        memcpy(destino.addr, mac, 6);
        mesh_data_t d = {.data = c, .size = sizeof(c), .proto = MESH_PROTO_BIN, .tos = MESH_TOS_P2P};
        esp_mesh_send(&destino, &d, MESH_DATA_P2P, NULL, 0);
        return;                                        /* aplicado: en revisar_vueltas() */
    } else {
        ESP_LOGW(TAG, "%s volvió con %s %s: no es la nueva (volvió a la anterior)", hw, tipo, version);
        cJSON *x = firmware(tipo, version, false);
        gw_responder(t.mac, "rechazado", t.command_id, "volvio_a_la_version_anterior", x);
        cJSON_Delete(x);
    }
    terminar(k);
}

/* ================================================================== */
/*  La imagen                                                          */
/* ================================================================== */
/* Tipo, versión y chip de la imagen que está en la partición */
static const char *leer_descripcion(void)
{
    uint8_t b[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)];
    if (esp_partition_read(part_imagen, 0, b, sizeof(b)) != ESP_OK) {
        return "error_de_flash";
    }
    const esp_image_header_t *h = (const esp_image_header_t *)b;
    esp_app_desc_t desc;
    memcpy(&desc, b + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(desc));
    if (h->magic != ESP_IMAGE_HEADER_MAGIC || desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "imagen_invalida";
    }
    desc.project_name[sizeof(desc.project_name) - 1] = '\0';
    desc.version[sizeof(desc.version) - 1] = '\0';
    if (!aura_tipo_valido(desc.project_name) || !aura_version_valida(desc.version)) {
        return "imagen_sin_tipo_o_version";
    }
    strlcpy(img_tipo, desc.project_name, sizeof(img_tipo));
    strlcpy(img_version, desc.version, sizeof(img_version));
    img_chip = h->chip_id;
    return NULL;
}

static bool sha_de_particion(uint32_t tamano, uint8_t sha[32])
{
    static uint8_t buf[4096];
    psa_hash_operation_t op = psa_hash_operation_init();
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return false;
    }
    for (uint32_t pos = 0; pos < tamano; pos += sizeof(buf)) {
        uint32_t n = tamano - pos < sizeof(buf) ? tamano - pos : sizeof(buf);
        if (esp_partition_read(part_imagen, pos, buf, n) != ESP_OK) {
            psa_hash_abort(&op);
            return false;
        }
        psa_hash_update(&op, buf, n);
    }
    size_t largo = 0;
    return psa_hash_finish(&op, sha, 32, &largo) == PSA_SUCCESS && largo == 32;
}

static void imagen_guardar_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_OTA, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "sha", img_sha, 32);
        nvs_set_u32(h, "tamano", img_tamano);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Lo que quedó descargado de antes (sobrevive a un reinicio): se verifica antes de usarlo */
static bool imagen_de_antes(const uint8_t sha[32])
{
    nvs_handle_t h;
    uint8_t s[32];
    size_t n = sizeof(s);
    uint32_t tamano = 0;
    if (nvs_open(NVS_OTA, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool hay = nvs_get_blob(h, "sha", s, &n) == ESP_OK && n == 32 && nvs_get_u32(h, "tamano", &tamano) == ESP_OK;
    nvs_close(h);
    uint8_t calculado[32];
    if (!hay || memcmp(s, sha, 32) != 0 || tamano == 0 || tamano > part_imagen->size ||
        !sha_de_particion(tamano, calculado) || memcmp(calculado, sha, 32) != 0 || leer_descripcion() != NULL) {
        return false;
    }
    memcpy(img_sha, sha, 32);
    img_tamano = tamano;
    img_ok = true;
    ESP_LOGI(TAG, "La imagen %s %s ya estaba descargada", img_tipo, img_version);
    return true;
}

static const char *descargar(const char *url, const uint8_t sha[32])
{
    img_ok = false;
    nvs_handle_t h;
    if (nvs_open(NVS_OTA, NVS_READWRITE, &h) == ESP_OK) {   /* lo que había deja de valer */
        nvs_erase_key(h, "sha");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Descargando %s", url);
    esp_http_client_config_t c = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&c);
    if (cli == NULL) {
        return "descarga_fallida";
    }
    const char *motivo = NULL;
    int64_t largo = 0;
    uint32_t total = 0;
    psa_hash_operation_t op = psa_hash_operation_init();
    bool hash_abierto = false;
    static uint8_t buf[4096];
    if (esp_http_client_open(cli, 0) != ESP_OK) {
        motivo = "descarga_fallida";
    } else if ((largo = esp_http_client_fetch_headers(cli)) <= 0 || esp_http_client_get_status_code(cli) != 200) {
        ESP_LOGW(TAG, "HTTP %d, largo %lld", esp_http_client_get_status_code(cli), (long long)largo);
        motivo = "descarga_fallida";
    } else if (largo > (int64_t)part_imagen->size) {
        motivo = "imagen_muy_grande";
    } else if (esp_partition_erase_range(part_imagen, 0, ((uint32_t)largo + 4095) & ~4095u) != ESP_OK) {
        motivo = "error_de_flash";
    } else if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        motivo = "error_de_sha256";
    } else {
        hash_abierto = true;
        while (total < (uint32_t)largo) {
            int n = esp_http_client_read(cli, (char *)buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            if (esp_partition_write(part_imagen, total, buf, n) != ESP_OK) {
                motivo = "error_de_flash";
                break;
            }
            psa_hash_update(&op, buf, n);
            total += n;
        }
        if (motivo == NULL && total != (uint32_t)largo) {
            ESP_LOGW(TAG, "Descarga cortada: %lu de %lld bytes", (unsigned long)total, (long long)largo);
            motivo = "descarga_fallida";
        }
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (motivo) {
        if (hash_abierto) {
            psa_hash_abort(&op);
        }
        return motivo;
    }
    uint8_t calculado[32];
    size_t n = 0;
    if (psa_hash_finish(&op, calculado, sizeof(calculado), &n) != PSA_SUCCESS || memcmp(calculado, sha, 32) != 0) {
        return "sha256_distinto";
    }
    if ((motivo = leer_descripcion()) != NULL) {
        return motivo;
    }
    memcpy(img_sha, sha, 32);
    img_tamano = total;
    img_ok = true;
    imagen_guardar_nvs();
    ESP_LOGI(TAG, "Imagen descargada y verificada: %s %s, %lu bytes", img_tipo, img_version, (unsigned long)total);
    return NULL;
}

/* ================================================================== */
/*  Transferencia a un nodo                                            */
/* ================================================================== */
static bool enviar(const uint8_t mac[6], size_t largo)
{
    mesh_addr_t destino;
    memcpy(destino.addr, mac, 6);
    mesh_data_t d = {.data = trama, .size = (uint16_t)largo, .proto = MESH_PROTO_BIN, .tos = MESH_TOS_P2P};
    return esp_mesh_send(&destino, &d, MESH_DATA_P2P, NULL, 0) == ESP_OK;
}

static void enviar_ventana(const uint8_t mac[6], uint32_t sesion, uint32_t desde)
{
    for (int k = 0; k < AURA_OTA_VENTANA; k++) {
        uint32_t pos = desde + (uint32_t)k * AURA_OTA_BLOQUE;
        if (pos >= img_tamano) {
            break;
        }
        uint32_t n = img_tamano - pos < AURA_OTA_BLOQUE ? img_tamano - pos : AURA_OTA_BLOQUE;
        uint8_t *c = aura_ota_cabecera(trama, AURA_OTA_DATOS, sesion);
        aura_u32_escribir(c, pos);
        if (esp_partition_read(part_imagen, pos, c + 4, n) != ESP_OK || !enviar(mac, AURA_OTA_CABECERA + 4 + n)) {
            break;                                     /* el nodo vuelve a pedir */
        }
    }
}

typedef enum { T_LISTO, T_YA_TIENE, T_RECHAZO, T_REINTENTAR } final_t;

static final_t transferir(int i, char *motivo, size_t tam)
{
    trabajo_t t = trabajos[i];
    char hw[AURA_HW_ID_BYTES];
    aura_hw_id_de_mac(t.mac, hw);
    uint32_t sesion = esp_random() | 1;
    aura_ota_oferta_t o = {.tamano = img_tamano, .chip = img_chip, .forzar = t.forzar};
    memcpy(o.sha256, img_sha, 32);
    strlcpy(o.tipo, img_tipo, sizeof(o.tipo));
    strlcpy(o.version, img_version, sizeof(o.version));
    aviso_t a;
    while (xQueueReceive(avisos, &a, 0) == pdTRUE) {   /* lo viejo no cuenta */
    }
    uint32_t inicio = ahora_ms(), ultima_oferta = 0, ultimo_pido = 0, tope = 0, ultimo_desde = 0, ultimo_sondeo = 0;
    int ofertas = 0;
    bool transmitido = false, aceptada = false;
    int cuarto = -1;
    ESP_LOGI(TAG, "%s: ofrezco %s %s (%lu bytes, sesión %08lx)", hw, img_tipo, img_version,
             (unsigned long)img_tamano, (unsigned long)sesion);
    while (true) {
        uint32_t ahora = ahora_ms();
        if (!aceptada && (ultima_oferta == 0 || ahora - ultima_oferta >= REPETIR_OFERTA_MS)) {
            ultima_oferta = ahora | 1;
            memcpy(aura_ota_cabecera(trama, AURA_OTA_OFERTA, sesion), &o, sizeof(o));
            if (enviar(t.mac, AURA_OTA_CABECERA + sizeof(o))) {
                if (!transmitido) {
                    transmitido = true;
                    gw_responder(t.mac, "transmitido", t.command_id, NULL, NULL);
                }
                if (++ofertas > OFERTAS_MAX) {
                    strlcpy(motivo, "nodo_no_responde", tam);   /* sin OTA (firmware viejo) o colgado */
                    return T_RECHAZO;
                }
            } else if (ahora - inicio >= ALCANZAR_MS) {
                strlcpy(motivo, "nodo_no_alcanzable", tam);
                return T_RECHAZO;
            }
        }
        if (xQueueReceive(avisos, &a, pdMS_TO_TICKS(200)) == pdTRUE && memcmp(a.mac, t.mac, 6) == 0 &&
            a.sesion == sesion) {
            if (a.op == AURA_OTA_PIDO && a.desde < img_tamano) {
                if (!aceptada) {
                    ESP_LOGI(TAG, "%s aceptó la oferta", hw);
                }
                aceptada = true;
                ultimo_pido = ahora_ms();
                ultimo_desde = a.desde;
                if (a.desde > tope) {
                    tope = a.desde;
                }
                progreso = (int)((uint64_t)tope * 100 / img_tamano);
                if (progreso / 25 != cuarto) {
                    cuarto = progreso / 25;
                    ESP_LOGI(TAG, "%s: %d %%", hw, progreso);
                    gw_publicar_status();
                }
                enviar_ventana(t.mac, sesion, a.desde);
            } else if (a.op == AURA_OTA_FIN) {
                if (a.resultado == AURA_OTA_LISTO) {
                    return T_LISTO;
                }
                if (a.resultado == AURA_OTA_YA_TIENE) {
                    return T_YA_TIENE;
                }
                strlcpy(motivo, a.motivo[0] ? a.motivo : "rechazado_por_el_nodo", tam);
                /* El nodo se reinició a mitad o dejó de recibir: se vuelve a empezar */
                bool corte = strcmp(motivo, "sesion_desconocida") == 0 || strcmp(motivo, "sin_datos_del_gateway") == 0;
                return corte ? T_REINTENTAR : T_RECHAZO;
            }
        }
        /* Un nodo que se reinició no pide más ni sabe de esta sesión: al recibir la ventana contesta
           sesion_desconocida y se vuelve a empezar enseguida, sin esperar AURA_OTA_CORTE_MS */
        if (aceptada && ahora_ms() - ultimo_pido >= SONDEO_MS && ahora_ms() - ultimo_sondeo >= SONDEO_MS) {
            ultimo_sondeo = ahora_ms();
            enviar_ventana(t.mac, sesion, ultimo_desde);
        }
        if (aceptada && ahora_ms() - ultimo_pido >= AURA_OTA_CORTE_MS) {
            aura_ota_cabecera(trama, AURA_OTA_CANCELAR, sesion);
            enviar(t.mac, AURA_OTA_CABECERA);
            strlcpy(motivo, "transferencia_cortada", tam);
            return T_REINTENTAR;
        }
    }
}

/* ================================================================== */
/*  El propio gateway                                                  */
/* ================================================================== */
static void actualizarme(int i)
{
    trabajo_t t = trabajos[i];
    if (img_chip != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        rechazar(i, "chip_distinto");
        return;
    }
    if (strcmp(img_tipo, aura_fw_tipo()) != 0 && !t.forzar) {
        rechazar(i, "tipo_distinto");
        return;
    }
    if (strcmp(img_tipo, aura_fw_tipo()) == 0 && strcmp(img_version, aura_fw_version()) == 0) {
        responder_fw(&t, "aplicado", img_tipo, img_version, true);
        terminar(i);
        return;
    }
    if (aura_fw_sin_confirmar()) {
        rechazar(i, "version_actual_sin_confirmar");
        return;
    }
    gw_responder(t.mac, "transmitido", t.command_id, NULL, NULL);
    const esp_partition_t *otra = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h;
    if (otra == NULL || img_tamano > otra->size || esp_ota_begin(otra, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        rechazar(i, "sin_particion_ota");
        return;
    }
    static uint8_t buf[4096];
    for (uint32_t pos = 0; pos < img_tamano; pos += sizeof(buf)) {
        uint32_t n = img_tamano - pos < sizeof(buf) ? img_tamano - pos : sizeof(buf);
        if (esp_partition_read(part_imagen, pos, buf, n) != ESP_OK || esp_ota_write(h, buf, n) != ESP_OK) {
            esp_ota_abort(h);
            rechazar(i, "error_de_flash");
            return;
        }
    }
    if (esp_ota_end(h) != ESP_OK) {
        rechazar(i, "imagen_invalida");
        return;
    }
    if (esp_ota_set_boot_partition(otra) != ESP_OK) {
        rechazar(i, "error_de_flash");
        return;
    }
    propio_t p = {0};
    strlcpy(p.command_id, t.command_id, sizeof(p.command_id));
    strlcpy(p.version, img_version, sizeof(p.version));
    nvs_handle_t nh;
    if (nvs_open(NVS_OTA, NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_blob(nh, "propio", &p, sizeof(p));
        nvs_commit(nh);
        nvs_close(nh);
    }
    responder_fw(&t, "recibido", img_tipo, img_version, false);
    ESP_LOGW(TAG, "Me actualizo a %s %s: me reinicio", img_tipo, img_version);
    vTaskDelay(pdMS_TO_TICKS(1500));                   /* que salgan las respuestas */
    esp_restart();
}

static propio_t propio;
static volatile bool propio_pendiente = false;  /* aplicado del propio gateway: cuando quede confirmado */

static void propio_olvidar(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_OTA, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "propio");
        nvs_commit(h);
        nvs_close(h);
    }
}

void ota_gw_broker_conectado(void)
{
    nvs_handle_t h;
    propio_t p;
    size_t n = sizeof(p);
    if (propio_pendiente || nvs_open(NVS_OTA, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    bool hay = nvs_get_blob(h, "propio", &p, &n) == ESP_OK && n == sizeof(p);
    nvs_close(h);
    if (!hay) {
        return;
    }
    if (strcmp(p.version, aura_fw_version()) == 0) {
        propio = p;                                    /* aplicado cuando lleve AURA_FW_ESTABLE_S andando */
        propio_pendiente = true;
        return;
    }
    cJSON *x = firmware(aura_fw_tipo(), aura_fw_version(), false);
    gw_responder(gw_mi_mac(), "rechazado", p.command_id, "volvio_a_la_version_anterior", x);
    cJSON_Delete(x);
    propio_olvidar();
}

static void revisar_propio(void)
{
    if (propio_pendiente && !aura_fw_sin_confirmar() && gw_mqtt_conectado()) {
        trabajo_t t = {0};
        memcpy(t.mac, gw_mi_mac(), 6);
        strlcpy(t.command_id, propio.command_id, sizeof(t.command_id));
        responder_fw(&t, "aplicado", aura_fw_tipo(), aura_fw_version(), false);
        propio_pendiente = false;
        propio_olvidar();
    }
}

/* ================================================================== */
/*  Tarea                                                              */
/* ================================================================== */
/* El próximo trabajo: primero los que usan la imagen ya descargada; el gateway, último de todos */
static int siguiente(void)
{
    int elegido = -1, propio = -1;
    bool otros = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < TRABAJOS_CAP; i++) {
        trabajo_t *t = &trabajos[i];
        if (t->estado == ESPERANDO_VUELTA) {
            otros = true;
        }
        if (t->estado != EN_COLA) {
            continue;
        }
        if (memcmp(t->mac, gw_mi_mac(), 6) == 0) {
            propio = i;
            continue;
        }
        otros = true;
        if (elegido < 0 || (img_ok && memcmp(t->sha, img_sha, 32) == 0 && memcmp(trabajos[elegido].sha, img_sha, 32) != 0)) {
            elegido = i;
        }
    }
    /* El gateway, cuando no queda nada más y ya pasó la gracia: si su pedido llegó primero de una
       tanda ("todas"), los demás entran antes de que se reinicie */
    if (elegido < 0 && propio >= 0 && !otros && ahora_ms() - trabajos[propio].desde_ms >= GRACIA_PROPIO_MS &&
        !aura_fw_sin_confirmar()) {
        elegido = propio;
    }
    if (elegido >= 0) {
        trabajos[elegido].estado = EN_CURSO;
    }
    xSemaphoreGive(lock);
    return elegido;
}

/* aplicado recién cuando la versión nueva lleva AURA_FW_ESTABLE_S andando (como la confirma el nodo):
   si se cuelga antes, vuelve a la anterior y eso se contesta rechazado */
static void revisar_vueltas(void)
{
    for (int i = 0; i < TRABAJOS_CAP; i++) {
        trabajo_t t = trabajos[i];
        if (t.estado != ESPERANDO_VUELTA) {
            continue;
        }
        if (t.volvio_ms && ahora_ms() - t.volvio_ms >= (AURA_FW_ESTABLE_S + 5) * 1000u) {
            char hw[AURA_HW_ID_BYTES];
            aura_hw_id_de_mac(t.mac, hw);
            ESP_LOGI(TAG, "%s: %s %s anda hace %d s: aplicada", hw, t.tipo, t.version, AURA_FW_ESTABLE_S);
            responder_fw(&t, "aplicado", t.tipo, t.version, false);
            terminar(i);
        } else if (!t.volvio_ms && ahora_ms() - t.listo_ms >= ESPERA_VUELTA_MS) {
            rechazar(i, "no_volvio");
        }
    }
}

static void atender(int i)
{
    trabajo_t t = trabajos[i];
    char hw[AURA_HW_ID_BYTES];
    aura_hw_id_de_mac(t.mac, hw);
    memcpy(mac_actual, t.mac, 6);
    progreso = 0;

    if (!(img_ok && memcmp(img_sha, t.sha, 32) == 0) && !imagen_de_antes(t.sha)) {
        const char *motivo = descargar(t.url, t.sha);
        if (motivo) {
            /* Los demás que pedían la misma imagen corren la misma suerte (no se baja N veces) */
            for (int k = 0; k < TRABAJOS_CAP; k++) {
                if (k != i && trabajos[k].estado == EN_COLA && memcmp(trabajos[k].sha, t.sha, 32) == 0) {
                    rechazar(k, motivo);
                }
            }
            rechazar(i, motivo);
            return;
        }
    }
    if (memcmp(t.mac, gw_mi_mac(), 6) == 0) {
        actualizarme(i);
        return;
    }
    /* Con lo que la placa informó en su reporte, sin mandarle nada */
    char fw[2 * AURA_OTA_TEXTO], tipo[AURA_OTA_TEXTO], version[AURA_OTA_TEXTO];
    if (gw_fw_de_nodo(t.mac, fw, sizeof(fw)) && partir_fw(fw, tipo, version)) {
        if (strcmp(tipo, img_tipo) != 0 && !t.forzar) {
            ESP_LOGW(TAG, "%s es %s y la imagen es %s", hw, tipo, img_tipo);
            cJSON *x = firmware(tipo, version, false);
            gw_responder(t.mac, "rechazado", t.command_id, "tipo_distinto", x);
            cJSON_Delete(x);
            terminar(i);
            return;
        }
        if (strcmp(tipo, img_tipo) == 0 && strcmp(version, img_version) == 0) {
            ESP_LOGI(TAG, "%s ya tiene %s %s", hw, tipo, version);
            responder_fw(&t, "aplicado", tipo, version, true);
            terminar(i);
            return;
        }
    }
    char motivo[AURA_OTA_MOTIVO_MAX + 1] = "";
    uint32_t t0 = ahora_ms();
    final_t r = transferir(i, motivo, sizeof(motivo));
    progreso = -1;
    switch (r) {
    case T_LISTO:
        ESP_LOGI(TAG, "%s tiene la imagen verificada (%lu s): espero que vuelva con %s", hw,
                 (unsigned long)((ahora_ms() - t0) / 1000), img_version);
        responder_fw(&t, "recibido", img_tipo, img_version, false);
        xSemaphoreTake(lock, portMAX_DELAY);
        trabajos[i].estado = ESPERANDO_VUELTA;
        trabajos[i].listo_ms = ahora_ms();
        trabajos[i].volvio_ms = 0;
        strlcpy(trabajos[i].tipo, img_tipo, sizeof(trabajos[i].tipo));
        strlcpy(trabajos[i].version, img_version, sizeof(trabajos[i].version));
        xSemaphoreGive(lock);
        break;
    case T_YA_TIENE:
        responder_fw(&t, "aplicado", img_tipo, img_version, true);
        terminar(i);
        break;
    case T_REINTENTAR:
        if (t.reintentos < REINTENTOS_MAX) {
            ESP_LOGW(TAG, "%s: %s, vuelvo a empezar (%d de %d)", hw, motivo, t.reintentos + 1, REINTENTOS_MAX);
            xSemaphoreTake(lock, portMAX_DELAY);
            trabajos[i].reintentos++;
            trabajos[i].estado = EN_COLA;
            xSemaphoreGive(lock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            break;
        }
        rechazar(i, motivo);
        break;
    default:
        rechazar(i, motivo);
        break;
    }
    gw_publicar_status();
}

static void tarea_ota(void *arg)
{
    while (true) {
        revisar_vueltas();
        revisar_propio();
        int i = siguiente();
        if (i < 0) {
            aviso_t a;
            xQueueReceive(avisos, &a, pdMS_TO_TICKS(500));   /* sin trabajo: lo que llegue se descarta */
            continue;
        }
        atender(i);
    }
}

void ota_gw_estado(cJSON *det)
{
    int en_cola = 0, esperando = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < TRABAJOS_CAP; i++) {
        en_cola += trabajos[i].estado == EN_COLA;
        esperando += trabajos[i].estado == ESPERANDO_VUELTA;
    }
    xSemaphoreGive(lock);
    int p = progreso;
    if (en_cola == 0 && esperando == 0 && p < 0) {
        return;
    }
    cJSON *o = cJSON_AddObjectToObject(det, "ota");
    cJSON_AddNumberToObject(o, "en_cola", en_cola);
    cJSON_AddNumberToObject(o, "esperando_vuelta", esperando);
    if (p >= 0) {
        char hw[AURA_HW_ID_BYTES];
        aura_hw_id_de_mac(mac_actual, hw);
        cJSON_AddStringToObject(o, "actual", hw);
        cJSON_AddNumberToObject(o, "progreso", p);
    }
    if (img_ok) {
        char fw[2 * AURA_OTA_TEXTO];
        snprintf(fw, sizeof(fw), "%s@%s", img_tipo, img_version);
        cJSON_AddStringToObject(o, "imagen", fw);
    }
}

void ota_gw_iniciar(void)
{
    lock = xSemaphoreCreateMutex();
    avisos = xQueueCreate(16, sizeof(aviso_t));
    psa_crypto_init();
    part_imagen = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, SUBTIPO_IMAGEN, "imagen");
    if (part_imagen == NULL) {
        ESP_LOGE(TAG, "No hay partición \"imagen\": no se puede actualizar a los nodos");
        return;
    }
    xTaskCreate(tarea_ota, "ota", 6144, NULL, 4, NULL);
}
