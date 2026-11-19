#include "almacenamiento.h"
#include "nvs.h"

typedef struct {
    uint32_t magic;
    hfr_config_t config;
    uint32_t crc;
} config_guardada_t;

/* Formato grabado por el prototipo con dos sondas. Solo se recupera la heladera. */
typedef struct {
    uint32_t magic;
    uint32_t intervalo_s;
    uint32_t intervalo_anterior;
    int16_t minimo[2];
    int16_t maximo[2];
    uint32_t crc;
} config_anterior_t;

static esp_err_t guardar(const char *clave, const void *datos, size_t tam)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("hfr_idf", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, clave, datos, tam);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t cargar(const char *clave, void *datos, size_t *tam)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("hfr_idf", NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(h, clave, datos, tam);
    nvs_close(h);
    return err;
}

esp_err_t hfr_guardar_config(const hfr_config_t *cfg)
{
    if (!hfr_config_valida(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }
    config_guardada_t dato = {.magic = 0x48464932, .config = *cfg};
    dato.crc = hfr_crc32(&dato, offsetof(config_guardada_t, crc));
    return guardar("config", &dato, sizeof(dato));
}

esp_err_t hfr_cargar_config(hfr_config_t *cfg)
{
    union {
        config_guardada_t actual;
        config_anterior_t anterior;
    } dato = {0};
    size_t tam = sizeof(dato);
    esp_err_t err = cargar("config", &dato, &tam);
    if (err != ESP_OK) {
        return err;
    }
    hfr_config_t recuperada;
    if (tam == sizeof(dato.actual) && dato.actual.magic == 0x48464932 &&
        dato.actual.crc == hfr_crc32(&dato.actual, offsetof(config_guardada_t, crc))) {
        recuperada = dato.actual.config;
    } else if (tam == sizeof(dato.anterior) && dato.anterior.magic == 0x48464931 &&
               dato.anterior.crc == hfr_crc32(&dato.anterior, offsetof(config_anterior_t, crc))) {
        recuperada = (hfr_config_t){
            .intervalo_s = dato.anterior.intervalo_s,
            .minimo = dato.anterior.minimo[0],
            .maximo = dato.anterior.maximo[0],
        };
    } else {
        return ESP_ERR_INVALID_CRC;
    }
    if (!hfr_config_valida(&recuperada)) {
        return ESP_ERR_INVALID_CRC;
    }
    *cfg = recuperada;
    return ESP_OK;
}

esp_err_t hfr_guardar_alimentacion(bool bateria)
{
    uint8_t valor = bateria;
    return guardar("bateria", &valor, sizeof(valor));
}

bool hfr_cargar_alimentacion(bool *bateria)
{
    uint8_t valor;
    size_t tam = sizeof(valor);
    if (cargar("bateria", &valor, &tam) != ESP_OK || tam != sizeof(valor) || valor > 1) {
        return false;
    }
    *bateria = valor;
    return true;
}
