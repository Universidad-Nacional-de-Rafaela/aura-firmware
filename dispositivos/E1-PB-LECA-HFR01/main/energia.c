#include "energia.h"
#include "almacenamiento.h"
#include "hardware.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nodo_mesh.h"
#include <math.h>

static bool nvs_lista;
static bool conocida;
static bool bateria;
static bool candidata;
static bool captura;
static bool pendiente_guardar;
static int64_t cambio_en;
static int64_t ultimo_guardado;
static hfr_corte_t corte;

static void guardar(void)
{
    if (!pendiente_guardar || !nvs_lista ||
        (ultimo_guardado != 0 && esp_timer_get_time() - ultimo_guardado < 1000000)) {
        return;
    }
    ultimo_guardado = esp_timer_get_time();
    hfr_corte_sellado(&corte);
    if (hfr_guardar_corte(&corte) == ESP_OK) {
        pendiente_guardar = false;
    } else {
        ESP_LOGE("ENERGIA", "No se pudo conservar el primer corte en NVS");
    }
}

void hfr_energia_iniciar(bool nvs_disponible)
{
    nvs_lista = nvs_disponible;
    conocida = false;
    bateria = false;
    captura = false;
    pendiente_guardar = false;
    ultimo_guardado = 0;
    corte = (hfr_corte_t){0};
    hfr_corte_t guardado;
    if (nvs_lista && hfr_cargar_corte(&guardado) == ESP_OK) {
        corte = guardado;
    }
    if (corte.activo) {
        ESP_LOGI("ENERGIA", "Corte protegido: UTC=%lu, sondas validas=%u; no se reenvia con otra fecha",
                 (unsigned long)corte.utc, corte.validas);
        for (unsigned i = 0; i < 2; ++i) {
            if ((corte.validas & (1U << i)) != 0) {
                ESP_LOGI("ENERGIA", "Captura sonda %u: %.2f C", i, corte.temperatura[i] / 100.0);
            }
        }
    }
    if (HFR_PIN_ALIMENTACION < 0) {
        ESP_LOGW("ENERGIA", "Entrada de alimentacion deshabilitada hasta confirmar el circuito");
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (HFR_PIN_ALIMENTACION < 0 ? 0 : HFR_PIN_ALIMENTACION),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = HFR_POWER_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    conocida = nvs_lista && hfr_cargar_alimentacion(&bateria);
    candidata = gpio_get_level(HFR_PIN_ALIMENTACION) != HFR_NIVEL_RED;
    cambio_en = esp_timer_get_time();
}

bool hfr_energia_actualizar(uint32_t utc, bool mesh_iniciada)
{
    guardar();
    if (HFR_PIN_ALIMENTACION < 0) {
        return false;
    }
    bool lectura = gpio_get_level(HFR_PIN_ALIMENTACION) != HFR_NIVEL_RED;
    int64_t ahora = esp_timer_get_time();
    if (lectura != candidata) {
        candidata = lectura;
        cambio_en = ahora;
    }
    if (ahora - cambio_en < 200000 ||
        (conocida && lectura == bateria && !(bateria && !corte.activo))) {
        return false;
    }
    bool notificar = conocida ? lectura != bateria : lectura;
    bool nuevo_corte = lectura && (!conocida || !bateria || !corte.activo);
    bateria = lectura;
    conocida = true;
    if (mesh_iniciada && notificar) {
        cJSON *detalles = cJSON_CreateObject();
        if (detalles != NULL) {
            cJSON_AddStringToObject(detalles, "alimentacion", bateria ? "bateria" : "red");
            nodo_mesh_alerta("energia", bateria ? "warning" : "info",
                             bateria ? "Alimentacion por bateria" : "Alimentacion de red recuperada", detalles);
            cJSON_Delete(detalles);
        }
    }
    if (nuevo_corte && !corte.activo) {
        corte = (hfr_corte_t){.utc = utc, .activo = 1};
        pendiente_guardar = true;
        captura = true;
        ultimo_guardado = 0;
        guardar();
    } else if (nuevo_corte) {
        ESP_LOGW("ENERGIA", "Se conserva el corte anterior: falta una API de confirmacion por muestra");
    }
    if (nvs_lista && hfr_guardar_alimentacion(bateria) != ESP_OK) {
        ESP_LOGE("ENERGIA", "No se pudo guardar el estado de alimentacion");
    }
    return nuevo_corte;
}

bool hfr_energia_bateria(void)
{
    return conocida && bateria;
}

void hfr_energia_completar(const float temperaturas[2], uint8_t validas)
{
    if (!captura) {
        return;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if ((validas & (1U << i)) != 0) {
            corte.temperatura[i] = (int16_t)lroundf(temperaturas[i] * 100);
        }
    }
    corte.validas = validas;
    captura = false;
    pendiente_guardar = true;
    guardar();
}
