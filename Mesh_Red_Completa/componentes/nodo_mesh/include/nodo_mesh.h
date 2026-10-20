/*
 * nodo_mesh: biblioteca de la HOJA de la mesh de AURA (ESP-IDF, contrato MQTT v4.0).
 *
 * Es lo que usa cada grupo para su dispositivo: el grupo escribe la medición, la
 * validación de su configuración y sus comandos; la biblioteca se ocupa del resto:
 *
 *  - Elección del padre por AULA: se cuelga del concentrador que anuncia su aula en el
 *    beacon y le manda cada dato UNA vez. Si no lo ve, plan B: se cuelga de la raíz o de
 *    otro concentrador y manda directo a la raíz; cada 30 s revisa si volvió el suyo.
 *  - El aula NO está en el firmware: se guarda en NVS y la asigna AURA con
 *    set_config {"aula": "..."}. Una placa recién grabada no tiene aula: entra por plan B
 *    y AURA la ve pendiente (cuarentena) hasta que la acepta y le asigna el aula.
 *  - Cada medición se guarda en una cola en RAM con su ingest_id ANTES del primer envío y
 *    sale de la cola solo cuando la raíz confirma que AURA la guardó. Sobrevive a cortes del
 *    WiFi, del broker, de AURA y del concentrador (no a un reinicio de la propia hoja).
 *  - Los eventos (resultado de un comando, reporte, alertas) se reintentan hasta que la raíz
 *    confirma que los publicó. Cuando vuelve el broker, la raíz pide un reporte nuevo.
 *  - Hora: la pide al arrancar y la ajusta con cada confirmación. Una medición tomada sin
 *    hora se fecha al enviarla, con el uptime.
 *  - La identidad es la MAC de la placa (hw_id = mac-...): todas las hojas de un mismo
 *    tipo llevan el mismo binario.
 *  - Es hoja: no acepta hijos y su AP casi no emite beacons.
 *
 * Uso mínimo (ver Mesh3_Hoja/main/main.c):
 *
 *   nodo_mesh_config_t cfg = {.mesh_id = {...}, .mesh_clave = "...",
 *                             .aplicar_config = ..., .describir_config = ...};
 *   nodo_mesh_iniciar(&cfg);
 *   cJSON *v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "temp_c", 24.5);
 *   nodo_mesh_medicion(v); cJSON_Delete(v);
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

/*
 * Valida params de un set_config y, solo si TODO valida, lo aplica y lo guarda (NVS).
 * Si algo no valida no toca nada, devuelve false y explica en motivo. Tiene que ignorar
 * "aula", que la maneja la biblioteca. NULL = el dispositivo no tiene parámetros.
 */
typedef bool (*nodo_mesh_aplicar_config_t)(const cJSON *params, char *motivo, size_t tam);

/* Agrega a config la configuración vigente. Incluir "intervalo_s": con eso la raíz
   infiere que el nodo se cayó (3 x intervalo sin datos). */
typedef void (*nodo_mesh_describir_config_t)(cJSON *config);

/* Comandos propios (un actuador, por ejemplo). local = vino del botón del concentrador,
   no de AURA. true = ejecutado. */
typedef bool (*nodo_mesh_ejecutar_comando_t)(const char *comando, const cJSON *params, bool local,
                                             char *motivo, size_t tam);

typedef struct {
    uint8_t     mesh_id[6];               /* de config_local.h: iguales en toda la mesh */
    const char *mesh_clave;
    int8_t      potencia_tx;              /* en pasos de 0,25 dBm; 0 = la de fábrica */
    const char *alimentacion;             /* "red", "bateria" o "desconocida" */
    nodo_mesh_aplicar_config_t   aplicar_config;
    nodo_mesh_describir_config_t describir_config;
    nodo_mesh_ejecutar_comando_t ejecutar_comando;
} nodo_mesh_config_t;

/* Llamar una vez, después de nvs_flash_init(). Arranca WiFi, mesh y sus tareas. */
esp_err_t nodo_mesh_iniciar(const nodo_mesh_config_t *cfg);

/* Guarda una medición en la cola. values lleva SOLO mediciones válidas (contrato §3.1):
   una sonda que falla no aparece. false = no se guardó (vacía o muy grande). */
bool nodo_mesh_medicion(const cJSON *values);

/* Alerta: la raíz la publica en hw/<hw_id>/alerts/<tipo> (tipo: [a-z_]{1,24}). */
void nodo_mesh_alerta(const char *tipo, const char *severity, const char *message, const cJSON *details);

/* Estado de una sonda en cada medición: manda la alerta "sensor" solo cuando cambia. */
void nodo_mesh_sonda(const char *campo, bool ok, const char *motivo_falla);

/* Manda un reporte de estado (por ejemplo, después de un cambio local de la config). */
void nodo_mesh_reportar(void);

bool nodo_mesh_conectada(void);
const char *nodo_mesh_aula(void);         /* "" = todavía sin aula */
uint32_t nodo_mesh_pendientes(void);
