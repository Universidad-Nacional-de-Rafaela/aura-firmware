#include "configuracion.h"
#include "energia.h"
#include "hardware.h"
#include "logica.h"
#include "bus_i2c.h"
#include "pantalla.h"
#include "reloj.h"
#include "sondas.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nodo_mesh.h"
#include "nvs_flash.h"
#include <math.h>
#include <string.h>

#if __has_include("config_local.h")
#include "config_local.h"
#else
#error "Copiar main/config_local.ejemplo.h como main/config_local.h y completar los datos de la mesh"
#endif

static const char *CAMPO = "temp_heladera_c";
static bool mesh_iniciada;

static void tarea_dispositivo(void *arg)
{
    (void)arg;
    float temperatura = NAN;
    bool valida = false;
    int64_t ultima_lectura = 0;
    int64_t ultimo_display = 0;
    hfr_alarma_t alarma = {0};
    hfr_config_t anterior = hfr_config_actual();
    int64_t desconexion_en = esp_timer_get_time();
    while (true) {
        int64_t ahora = esp_timer_get_time();
        hfr_config_t cfg = hfr_config_actual();
        hfr_energia_actualizar(mesh_iniciada);
        if (cfg.minimo != anterior.minimo || cfg.maximo != anterior.maximo) {
            alarma = (hfr_alarma_t){0};
        }
        anterior = cfg;
        if (ultima_lectura == 0 || ahora - ultima_lectura >= (int64_t)cfg.intervalo_s * 1000000) {
            ultima_lectura = ahora;
            temperatura = NAN;
            bool leida = hfr_sonda_leer(&temperatura);
            hfr_sonda_estado_t estado = hfr_clasificar(temperatura, leida);
            valida = estado == HFR_SONDA_OK;
            if (valida) {
                hfr_alarma_actualizar(&alarma, temperatura, cfg.minimo, cfg.maximo);
                ESP_LOGI("SONDA", "%s = %.2f C", CAMPO, (double)temperatura);
                cJSON *medicion = cJSON_CreateObject();
                if (medicion != NULL &&
                    cJSON_AddNumberToObject(medicion, CAMPO, round((double)temperatura * 100) / 100) != NULL) {
                    if (!mesh_iniciada || !nodo_mesh_medicion(medicion)) {
                        ESP_LOGE("MEDICION", "Lectura no encolada; revisar inicializacion o memoria");
                    }
                } else {
                    ESP_LOGE("MEDICION", "Sin memoria para preparar la lectura");
                }
                cJSON_Delete(medicion);
            }
            if (mesh_iniciada) {
                nodo_mesh_sonda(CAMPO, valida,
                                estado == HFR_SIN_RESPUESTA ? "sin_respuesta" : "fuera_de_rango");
            }
        }

        bool conectada = mesh_iniciada && nodo_mesh_conectada();
        if (conectada) {
            desconexion_en = ahora;
        }
        hfr_indicadores(alarma.activa != 0, conectada);
        if (ahora - ultimo_display >= 1000000) {
            ultimo_display = ahora;
            bool oscura = hfr_energia_bateria() ||
                          ahora - desconexion_en >= (int64_t)HFR_OSCURO_TRAS_MS * 1000;
            uint32_t pendientes = mesh_iniciada ? nodo_mesh_pendientes() : 0;
            hfr_pantalla_actualizar(temperatura, valida, conectada, pendientes,
                                   hfr_energia_bateria(), hfr_reloj_utc(), oscura);
            hfr_reloj_mantener(conectada);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW("NVS", "NVS incompatible o sin espacio: se restauran los ajustes iniciales");
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    bool nvs_lista = err == ESP_OK;
    if (!nvs_lista) {
        ESP_LOGE("NVS", "No disponible: %s. Solo diagnostico local", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(hfr_config_iniciar(nvs_lista));
    hfr_i2c_iniciar();
    hfr_reloj_iniciar();
    hfr_pantalla_iniciar();
    hfr_sondas_iniciar();
    hfr_energia_iniciar(nvs_lista);

    nodo_mesh_config_t cfg = {
        .mesh_id = {MESH_ID_BYTES},
        .mesh_clave = MESH_AP_PASS,
        .potencia_tx = HFR_POTENCIA_TX,
        .alimentacion = "desconocida",
        .aplicar_config = hfr_config_aplicar,
        .describir_config = hfr_config_describir,
    };
    const uint8_t vacio[6] = {0};
    bool credenciales = memcmp(cfg.mesh_id, vacio, sizeof(vacio)) != 0 &&
                        strlen(cfg.mesh_clave) >= 8 && strlen(cfg.mesh_clave) <= 63 &&
                        strcmp(cfg.mesh_clave, "CAMBIAR") != 0;
    if (nvs_lista && credenciales) {
        err = nodo_mesh_iniciar(&cfg);
        mesh_iniciada = err == ESP_OK;
        if (!mesh_iniciada) {
            ESP_LOGE("MESH", "No se pudo iniciar: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE("MESH", "Sin datos de mesh validos o sin NVS: solo diagnostico local");
    }
    if (xTaskCreate(tarea_dispositivo, "heladera", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE("HFR01", "No hay memoria para iniciar las mediciones");
    }
}
