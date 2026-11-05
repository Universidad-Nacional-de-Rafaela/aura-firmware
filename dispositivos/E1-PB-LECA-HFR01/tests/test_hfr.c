#include "configuracion.h"
#include "almacenamiento.h"
#include "nvs.h"
#include "energia.h"
#include "driver/gpio.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* NVS transaccional en memoria: un commit fallido no cambia el dato durable. */
typedef struct { unsigned char datos[64]; size_t tam; } registro_t;
static registro_t registros[3];
static registro_t pendiente;
static unsigned clave_pendiente;
static bool fallar_escritura;
static bool fallar_commit;
static unsigned escrituras;
static int nivel_red = 1;
static int64_t reloj_us;
static unsigned alertas;

esp_err_t gpio_config(const gpio_config_t *cfg) { (void)cfg; return ESP_OK; }
int gpio_get_level(int pin) { assert(pin == 1); return nivel_red; }
int64_t esp_timer_get_time(void) { return reloj_us; }
void nodo_mesh_alerta(const char *tipo, const char *severity, const char *message, const cJSON *details)
{
    (void)severity;
    (void)message;
    assert(strcmp(tipo, "energia") == 0);
    assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(details, "alimentacion")));
    ++alertas;
}

static unsigned indice(const char *clave)
{
    if (strcmp(clave, "config") == 0) return 0;
    if (strcmp(clave, "corte") == 0) return 1;
    assert(strcmp(clave, "bateria") == 0);
    return 2;
}

const char *esp_err_to_name(esp_err_t err) { (void)err; return "doble"; }
esp_err_t nvs_open(const char *nombre, int modo, nvs_handle_t *h)
{
    assert(strcmp(nombre, "hfr_idf") == 0);
    (void)modo;
    *h = 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *clave, void *datos, size_t *tam)
{
    (void)h;
    registro_t *r = &registros[indice(clave)];
    if (r->tam == 0) return ESP_ERR_NVS_NOT_FOUND;
    if (*tam < r->tam) return ESP_ERR_INVALID_SIZE;
    memcpy(datos, r->datos, r->tam);
    *tam = r->tam;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *clave, const void *datos, size_t tam)
{
    (void)h;
    ++escrituras;
    if (fallar_escritura) return ESP_FAIL;
    assert(tam <= sizeof(pendiente.datos));
    memcpy(pendiente.datos, datos, tam);
    pendiente.tam = tam;
    clave_pendiente = indice(clave);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    if (fallar_commit) return ESP_FAIL;
    registros[clave_pendiente] = pendiente;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }

static void configuracion(void)
{
    assert(hfr_config_iniciar(true) == ESP_OK);
    hfr_config_t cfg = hfr_config_actual();
    assert(cfg.intervalo_s == 60 && cfg.freezer_s == 300);
    const char *invalidos[] = {
        "{}", "[]", "null", "{\"intervalo_s\":4}", "{\"f_s\":86401}",
        "{\"intervalo_s\":5.5}", "{\"f_s\":true}", "{\"hlo\":\"2\"}",
        "{\"hlo\":-55.01}", "{\"hhi\":125.01}", "{\"flo\":-20.001}",
        "{\"hlo\":6}", "{\"flo\":-10,\"fhi\":-15}",
        "{\"f_s\":5,\"desconocido\":0}", "{\"r_s\":300}",
        "{\"f_s\":5,\"f_s\":6}", "{\"intervalo_s\":1e300}",
    };
    char motivo[80];
    for (size_t i = 0; i < sizeof(invalidos) / sizeof(*invalidos); ++i) {
        cJSON *params = cJSON_Parse(invalidos[i]);
        assert(!hfr_config_aplicar(params, motivo, sizeof(motivo)));
        assert(motivo[0] != 0);
        hfr_config_t despues = hfr_config_actual();
        assert(memcmp(&cfg, &despues, sizeof(cfg)) == 0);
        assert(escrituras == 0);
        cJSON_Delete(params);
    }
    cJSON *params = cJSON_Parse("{\"intervalo_s\":5,\"hlo\":-55,\"hhi\":125,\"aula\":\"aula1\"}");
    assert(hfr_config_aplicar(params, motivo, sizeof(motivo)));
    assert(escrituras == 1);
    cfg = hfr_config_actual();
    assert(cfg.intervalo_s == 5 && cfg.freezer_s == 300 && cfg.minimo[0] == -5500);
    assert(hfr_config_aplicar(params, motivo, sizeof(motivo)));
    assert(escrituras == 1); /* No desgastar flash si no cambió. */
    cJSON_Delete(params);
    params = cJSON_Parse("{\"f_s\":86400,\"flo\":-25.25,\"fhi\":-14.75}");
    fallar_escritura = true;
    assert(!hfr_config_aplicar(params, motivo, sizeof(motivo)));
    fallar_escritura = false;
    fallar_commit = true;
    assert(!hfr_config_aplicar(params, motivo, sizeof(motivo)));
    fallar_commit = false;
    hfr_config_t despues = hfr_config_actual();
    assert(memcmp(&cfg, &despues, sizeof(cfg)) == 0);
    hfr_config_t durable;
    assert(hfr_cargar_config(&durable) == ESP_OK);
    assert(memcmp(&cfg, &durable, sizeof(cfg)) == 0);
    assert(hfr_config_aplicar(params, motivo, sizeof(motivo)));
    cJSON_Delete(params);
    cfg = hfr_config_actual();
    assert(cfg.freezer_s == 86400 && cfg.minimo[1] == -2525 && cfg.maximo[1] == -1475);
    assert(hfr_cargar_config(&durable) == ESP_OK);
    assert(memcmp(&cfg, &durable, sizeof(cfg)) == 0);
    cJSON *reporte = cJSON_CreateObject();
    hfr_config_describir(reporte);
    assert(cJSON_GetArraySize(reporte) == 6);
    assert(cJSON_GetObjectItemCaseSensitive(reporte, "f_s")->valuedouble == 86400);
    cJSON_Delete(reporte);
    registros[0].datos[7] ^= 1;
    assert(hfr_cargar_config(&durable) == ESP_ERR_INVALID_CRC);
    puts("OK configuracion: 17 parches invalidos, atomicidad, limites, NVS y CRC");
}

static void sondas(void)
{
    float invalidas[] = {NAN, INFINITY, -INFINITY, -127, 85, -55.1f, 125.1f};
    for (size_t i = 0; i < sizeof(invalidas) / sizeof(*invalidas); ++i) {
        assert(hfr_clasificar(invalidas[i], true) != HFR_SONDA_OK);
        assert(!hfr_fuera_umbral(invalidas[i], 200, 600));
    }
    assert(hfr_clasificar(4, false) == HFR_SIN_RESPUESTA);
    assert(hfr_clasificar(-55, true) == HFR_SONDA_OK);
    assert(hfr_clasificar(125, true) == HFR_SONDA_OK);
    assert(!hfr_fuera_umbral(2, 200, 600));
    assert(!hfr_fuera_umbral(6, 200, 600));
    assert(hfr_fuera_umbral(6.125f, 200, 600));
    assert(hfr_fuera_umbral(-26, -2500, -1500));
    puts("OK sondas: centinelas, limites y alarma local");
}

static void corte(void)
{
    hfr_corte_t dato = {.utc = 1792490400, .activo = 1};
    hfr_corte_sellado(&dato);
    assert(hfr_guardar_corte(&dato) == ESP_OK); /* Incluso con ambas sondas ausentes. */
    hfr_corte_t copia;
    assert(hfr_cargar_corte(&copia) == ESP_OK);
    assert(copia.utc == dato.utc && copia.validas == 0);
    dato.validas = 3;
    dato.temperatura[0] = 425;
    dato.temperatura[1] = -1800;
    hfr_corte_sellado(&dato);
    assert(hfr_guardar_corte(&dato) == ESP_OK);
    assert(hfr_cargar_corte(&copia) == ESP_OK);
    assert(memcmp(&dato, &copia, sizeof(dato)) == 0);
    registros[1].datos[4] ^= 1;
    assert(hfr_cargar_corte(&copia) == ESP_ERR_INVALID_CRC);
    dato.validas = 4;
    hfr_corte_sellado(&dato);
    assert(hfr_guardar_corte(&dato) == ESP_ERR_INVALID_ARG);
    bool bateria;
    assert(!hfr_cargar_alimentacion(&bateria));
    assert(hfr_guardar_alimentacion(true) == ESP_OK);
    assert(hfr_cargar_alimentacion(&bateria) && bateria);
    puts("OK corte: marca horaria sin sondas, persistencia y deteccion de corrupcion");
}

static void reloj(void)
{
    const uint32_t fechas[] = {1700000000, 1709164800, 1792490400, 4102444799UL};
    for (size_t i = 0; i < sizeof(fechas) / sizeof(*fechas); ++i) {
        uint8_t r[7];
        uint32_t utc;
        assert(hfr_rtc_codificar(fechas[i], r));
        assert(hfr_rtc_decodificar(r, &utc) && utc == fechas[i]);
    }
    uint8_t r[7] = {0,0,0,5,0x29,0x02,0x24};
    uint32_t utc;
    assert(hfr_rtc_decodificar(r, &utc) && utc == 1709164800);
    r[6] = 0x25; /* 29/2/2025 no existe. */
    assert(!hfr_rtc_decodificar(r, &utc));
    r[6] = 0x24;
    r[2] = 0x52; /* 12 AM. */
    assert(hfr_rtc_decodificar(r, &utc) && utc == 1709164800);
    r[2] = 0x72; /* 12 PM. */
    assert(hfr_rtc_decodificar(r, &utc) && utc == 1709208000);
    r[0] = 0x6a;
    assert(!hfr_rtc_decodificar(r, &utc));
    assert(!hfr_rtc_codificar(0, r));
    puts("OK RTC: UTC, bisiestos, 12/24 horas y BCD invalido");
}

static void energia(void)
{
    memset(registros, 0, sizeof(registros));
    reloj_us = 2000000;
    nivel_red = 1;
    hfr_energia_iniciar(true);
    reloj_us += 200001;
    assert(!hfr_energia_actualizar(1792490400, true));
    assert(!hfr_energia_bateria());
    nivel_red = 0;
    assert(!hfr_energia_actualizar(1792490400, true));
    reloj_us += 100000;
    nivel_red = 1; /* Rebote, no es un corte. */
    assert(!hfr_energia_actualizar(1792490400, true));
    nivel_red = 0;
    assert(!hfr_energia_actualizar(1792490400, true));
    reloj_us += 200001;
    assert(hfr_energia_actualizar(1792490400, true));
    assert(hfr_energia_bateria());
    hfr_corte_t dato;
    assert(hfr_cargar_corte(&dato) == ESP_OK && dato.utc == 1792490400 && dato.validas == 0);
    float temperaturas[2] = {4.5f, -18};
    hfr_energia_completar(temperaturas, 3);
    reloj_us += 1000001;
    assert(!hfr_energia_actualizar(1792490401, true));
    assert(hfr_cargar_corte(&dato) == ESP_OK && dato.temperatura[0] == 450 && dato.validas == 3);
    hfr_energia_iniciar(true); /* Reinicio con batería: no cambia el primer corte. */
    reloj_us += 200001;
    assert(!hfr_energia_actualizar(1792490402, true));
    assert(hfr_energia_bateria());
    nivel_red = 1;
    assert(!hfr_energia_actualizar(1792490500, true));
    reloj_us += 200001;
    assert(!hfr_energia_actualizar(1792490500, true));
    nivel_red = 0;
    assert(!hfr_energia_actualizar(1792490600, true));
    reloj_us += 200001;
    assert(hfr_energia_actualizar(1792490600, true));
    assert(hfr_cargar_corte(&dato) == ESP_OK && dato.utc == 1792490400 && dato.temperatura[0] == 450);
    assert(alertas == 3); /* Primer corte, red, segundo corte; no inventar recuperacion al iniciar. */
    /* Un registro corrupto no bloquea la captura siguiente. */
    registros[1].datos[4] ^= 1;
    hfr_energia_iniciar(true);
    reloj_us += 200001;
    fallar_escritura = true;
    assert(hfr_energia_actualizar(1792490700, false));
    hfr_energia_completar(temperaturas, 0);
    fallar_escritura = false;
    reloj_us += 1000001;
    assert(!hfr_energia_actualizar(1792490701, false));
    assert(hfr_cargar_corte(&dato) == ESP_OK && dato.utc == 1792490700 && dato.validas == 0);
    puts("OK energia: rebotes, primer corte protegido, reinicio, segundo corte y recuperacion de NVS");
}

int main(void)
{
    assert(hfr_crc32("123456789", 9) == 0xcbf43926);
    configuracion();
    sondas();
    corte();
    reloj();
    energia();
    puts("Todas las pruebas de host del nodo pasaron");
    return 0;
}
