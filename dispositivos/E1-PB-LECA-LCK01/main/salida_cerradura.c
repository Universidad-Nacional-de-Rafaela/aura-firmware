#include "salida_cerradura.h"
#include "cerradura_pulso.h"
#include "pines.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "CERRADURA";

/* Si la logica corta justo en el borde, el corte de hardware llega apenas despues del de ella */
#define MARGEN_CORTE_MS 150

static esp_timer_handle_t temporizador[CANT_LOCKERS];
static volatile bool energizada[CANT_LOCKERS];

void salida_cerradura_reposo_inicial(void)
{
    for (int i = 0; i < CANT_LOCKERS; i++) {
        /* El nivel se escribe ANTES de pasar el pin a salida: sale directo en reposo. Sin
           gpio_reset_pin(), que prende el pull-up interno y levantaria el gate un instante. */
        gpio_set_level(PIN_CERRADURA[i], 0);
        const gpio_config_t c = {
            .pin_bit_mask = 1ULL << PIN_CERRADURA[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&c);
        gpio_set_level(PIN_CERRADURA[i], 0);
        energizada[i] = false;
    }
}

static void al_vencer(void *arg)
{
    int i = (int)(intptr_t)arg;
    gpio_set_level(PIN_CERRADURA[i], 0);
    energizada[i] = false;
    ESP_EARLY_LOGW(TAG, "Corte por temporizador: cerradura %d trabada", i + 1);
}

void salida_cerradura_iniciar(void)
{
    for (int i = 0; i < CANT_LOCKERS; i++) {
        const esp_timer_create_args_t args = {
            .callback = al_vencer,
            .arg = (void *)(intptr_t)i,
            .name = "corte_cerradura",
            .dispatch_method = ESP_TIMER_TASK,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &temporizador[i]));
    }
}

void salida_cerradura_energizar(int i, uint32_t ms)
{
    if (i < 0 || i >= CANT_LOCKERS) return;
    if (ms > PULSO_TOPE_MS) ms = PULSO_TOPE_MS;       /* tope duro, pase lo que pase arriba */
    if (ms == 0) return;
    esp_timer_stop(temporizador[i]);                   /* ESP_ERR_INVALID_STATE si no corria: no importa */
    /* Primero se arma el corte y despues se energiza: nunca hay un pulso sin temporizador */
    ESP_ERROR_CHECK(esp_timer_start_once(temporizador[i], (uint64_t)(ms + MARGEN_CORTE_MS) * 1000));
    gpio_set_level(PIN_CERRADURA[i], 1);
    energizada[i] = true;
}

void salida_cerradura_soltar(int i)
{
    if (i < 0 || i >= CANT_LOCKERS) return;
    gpio_set_level(PIN_CERRADURA[i], 0);
    energizada[i] = false;
    esp_timer_stop(temporizador[i]);
}

bool salida_cerradura_energizada(int i)
{
    return i >= 0 && i < CANT_LOCKERS && energizada[i];
}
