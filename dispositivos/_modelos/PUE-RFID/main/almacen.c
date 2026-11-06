#include "almacen.h"
#include "puerta_logica.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "ALMACEN";
static nvs_handle_t h;
static bool abierto;

bool almacen_iniciar(void)
{
    esp_err_t e = nvs_open("puerta", NVS_READWRITE, &h);
    abierto = (e == ESP_OK);
    if (!abierto) ESP_LOGE(TAG, "No abre NVS: %s", esp_err_to_name(e));
    return abierto;
}

/* Escribe un blob solo si es distinto de lo que ya hay */
static bool blob_si_cambia(const char *clave, const void *datos, size_t n)
{
    if (!abierto) return false;
    size_t actual = 0;
    if (nvs_get_blob(h, clave, NULL, &actual) == ESP_OK && actual == n) {
        uint8_t viejo[sizeof(tarjeta_t) * LISTA_MAX];
        if (n <= sizeof(viejo) && nvs_get_blob(h, clave, viejo, &actual) == ESP_OK && memcmp(viejo, datos, n) == 0) {
            return true;                       /* igual: no se escribe */
        }
    }
    return nvs_set_blob(h, clave, datos, n) == ESP_OK && nvs_commit(h) == ESP_OK;
}

static bool u32_si_cambia(const char *clave, uint32_t valor)
{
    if (!abierto) return false;
    uint32_t actual;
    if (nvs_get_u32(h, clave, &actual) == ESP_OK && actual == valor) return true;
    return nvs_set_u32(h, clave, valor) == ESP_OK && nvs_commit(h) == ESP_OK;
}

void almacen_cargar_lista(lista_t *l)
{
    lista_vaciar(l);
    if (!abierto) return;
    uint8_t buf[sizeof(tarjeta_t) * LISTA_MAX];
    size_t n = sizeof(buf);
    if (nvs_get_blob(h, "tarjetas", buf, &n) != ESP_OK) return;
    /* Cada elemento se vuelve a validar: una entrada corrupta no entra a la lista */
    for (size_t i = 0; i + sizeof(tarjeta_t) <= n; i += sizeof(tarjeta_t)) {
        const tarjeta_t *t = (const tarjeta_t *)(buf + i);
        if (lista_agregar(l, t->uid, t->largo) != LISTA_OK) {
            ESP_LOGW(TAG, "Llavero descartado de NVS (invalido o repetido)");
        }
    }
}

bool almacen_guardar_lista(const lista_t *l)
{
    return blob_si_cambia("tarjetas", l->t, sizeof(tarjeta_t) * l->n);
}

bool almacen_cargar_armada(void)
{
    uint8_t v = 0;
    if (abierto) nvs_get_u8(h, "armada", &v);
    return v != 0;
}

bool almacen_guardar_armada(bool armada)
{
    if (!abierto) return false;
    uint8_t actual;
    if (nvs_get_u8(h, "armada", &actual) == ESP_OK && actual == (armada ? 1 : 0)) return true;
    return nvs_set_u8(h, "armada", armada ? 1 : 0) == ESP_OK && nvs_commit(h) == ESP_OK;
}

void almacen_cargar_config(config_puerta_t *c)
{
    c->pulso_ms = PULSO_DEFECTO_MS;
    c->aviso_s = AVISO_DEFECTO_S;
    c->sensor = SENSOR_REED;
    if (!abierto) return;
    uint32_t v;
    /* Lo guardado se vuelve a validar contra el rango: si esta fuera, queda el valor por defecto */
    if (nvs_get_u32(h, "pulso_ms", &v) == ESP_OK && v >= PULSO_MINIMO_MS && v <= PULSO_TOPE_MS) c->pulso_ms = (int32_t)v;
    if (nvs_get_u32(h, "aviso_s", &v) == ESP_OK && v >= AVISO_MIN_S && v <= AVISO_MAX_S) c->aviso_s = (int32_t)v;
    if (nvs_get_u32(h, "sensor", &v) == ESP_OK && v <= SENSOR_BOTON) c->sensor = (sensor_tipo_t)v;
}

bool almacen_guardar_config(const config_puerta_t *c)
{
    bool ok = u32_si_cambia("pulso_ms", (uint32_t)c->pulso_ms);
    ok = u32_si_cambia("aviso_s", (uint32_t)c->aviso_s) && ok;
    ok = u32_si_cambia("sensor", (uint32_t)c->sensor) && ok;
    return ok;
}
