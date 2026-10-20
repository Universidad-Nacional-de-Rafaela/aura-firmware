#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cola.h"

static const char *TAG = "COLA";
static cola_muestra_t *buf = NULL;
static SemaphoreHandle_t lock = NULL;
static uint32_t capacidad = 0;
static uint32_t cabeza = 0;                  /* la más vieja */
static uint32_t cantidad = 0;
static uint32_t descartadas = 0;

esp_err_t cola_abrir(void)
{
    lock = xSemaphoreCreateMutex();
    /* Si no hay RAM para todas, se pide de a menos: mejor una cola chica que ninguna */
    for (capacidad = COLA_CAPACIDAD; capacidad >= 16 && buf == NULL; capacidad /= 2) {
        buf = heap_caps_malloc(capacidad * sizeof(cola_muestra_t), MALLOC_CAP_8BIT);
        if (buf != NULL) {
            break;
        }
    }
    if (buf == NULL) {
        capacidad = 0;
        ESP_LOGE(TAG, "Sin RAM para la cola");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Cola en RAM: %lu mediciones (%lu KB)", (unsigned long)capacidad,
             (unsigned long)(capacidad * sizeof(cola_muestra_t) / 1024));
    return ESP_OK;
}

bool cola_guardar(const cola_muestra_t *m)
{
    if (buf == NULL) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (cantidad == capacidad) {             /* llena: se pierde la más vieja */
        cabeza = (cabeza + 1) % capacidad;
        cantidad--;
        descartadas++;
    }
    buf[(cabeza + cantidad) % capacidad] = *m;
    cantidad++;
    xSemaphoreGive(lock);
    return true;
}

bool cola_mirar(cola_muestra_t *m)
{
    if (buf == NULL) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = cantidad > 0;
    if (ok) {
        *m = buf[cabeza];
    }
    xSemaphoreGive(lock);
    return ok;
}

bool cola_sacar(void)
{
    if (buf == NULL) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = cantidad > 0;
    if (ok) {
        cabeza = (cabeza + 1) % capacidad;
        cantidad--;
    }
    xSemaphoreGive(lock);
    return ok;
}

uint32_t cola_cantidad(void)
{
    return cantidad;
}

uint32_t cola_descartadas(void)
{
    return descartadas;
}

uint32_t cola_capacidad(void)
{
    return capacidad;
}
