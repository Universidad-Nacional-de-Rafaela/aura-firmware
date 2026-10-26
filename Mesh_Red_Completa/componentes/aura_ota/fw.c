#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "aura_ota.h"
#include "aura_fw.h"

static const char *TAG = "AURA_FW";
static volatile bool sin_confirmar = false;
static volatile bool hablo = false;             /* ya habló con la raíz (o el gateway con el broker) */
static esp_timer_handle_t vencimiento = NULL, revision = NULL;
static void (*antes_de_volver)(void) = NULL;
static TaskHandle_t tarea_volver = NULL;

/* La vuelta atrás corre en una tarea propia: antes_de_volver() espera eventos y escribe en flash,
   y la tarea de esp_timer tiene poca pila y frena a los demás timers mientras tanto (revisión #24) */
static void volver(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGE(TAG, "La versión nueva no se confirmó en %d s: vuelvo a la anterior", AURA_FW_VERIFICAR_S);
    if (antes_de_volver) {
        antes_de_volver();
    }
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();                                     /* por si no había anterior: el bootloader decide */
}

static void no_se_confirmo(void *arg)
{
    if (tarea_volver) {
        xTaskNotifyGive(tarea_volver);
    } else {
        esp_ota_mark_app_invalid_rollback_and_reboot();
        esp_restart();
    }
}

static void marcar_buena(void)
{
    sin_confirmar = false;
    esp_timer_stop(vencimiento);
    esp_timer_stop(revision);
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "Versión %s confirmada: ya no vuelve a la anterior", aura_fw_version());
    }
}

/* Cada 5 s: habló y además lleva AURA_FW_ESTABLE_S andando sin colgarse */
static void revisar(void *arg)
{
    if (sin_confirmar && hablo && esp_timer_get_time() >= (int64_t)AURA_FW_ESTABLE_S * 1000000) {
        marcar_buena();
    }
}

void aura_fw_iniciar(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware %s versión %s", d->project_name, d->version);
    if (!aura_tipo_valido(d->project_name)) {
        ESP_LOGE(TAG, "El tipo \"%s\" no cumple la regla: project(aura-<uso>) con a-z, 0-9 y \"-\"", d->project_name);
    }
    if (!aura_version_valida(d->version)) {
        ESP_LOGE(TAG, "La versión \"%s\" no cumple la regla: set(PROJECT_VER \"mayor.menor.parche\")", d->version);
    }
    esp_ota_img_states_t estado;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &estado) == ESP_OK &&
        estado == ESP_OTA_IMG_PENDING_VERIFY) {
        sin_confirmar = true;
        ESP_LOGW(TAG, "Versión recién instalada: si en %d min no se confirma, vuelve a la anterior",
                 AURA_FW_VERIFICAR_S / 60);
        const esp_timer_create_args_t a = {.callback = no_se_confirmo, .name = "fw_verificar"};
        const esp_timer_create_args_t b = {.callback = revisar, .name = "fw_revisar"};
        xTaskCreate(volver, "fw_volver", 4096, NULL, 5, &tarea_volver);
        if (esp_timer_create(&a, &vencimiento) == ESP_OK && esp_timer_create(&b, &revision) == ESP_OK) {
            esp_timer_start_once(vencimiento, (uint64_t)AURA_FW_VERIFICAR_S * 1000000);
            esp_timer_start_periodic(revision, 5000000);
        }
    }
}

const char *aura_fw_tipo(void)
{
    return esp_app_get_description()->project_name;
}

const char *aura_fw_version(void)
{
    return esp_app_get_description()->version;
}

void aura_fw_texto(char *salida, size_t tam)
{
    snprintf(salida, tam, "%s@%s", aura_fw_tipo(), aura_fw_version());
}

void aura_fw_confirmar(void)
{
    if (!sin_confirmar || hablo) {
        return;
    }
    hablo = true;
    ESP_LOGI(TAG, "La versión %s habla con la raíz: queda confirmada a los %d s de andar, si no se cuelga",
             aura_fw_version(), AURA_FW_ESTABLE_S);
}

bool aura_fw_sin_confirmar(void)
{
    return sin_confirmar;
}

void aura_fw_antes_de_volver(void (*cb)(void))
{
    antes_de_volver = cb;
}
