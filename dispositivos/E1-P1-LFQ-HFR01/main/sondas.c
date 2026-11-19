#include "sondas.h"
#include "hardware.h"
#include "onewire_bus.h"
#include "ds18b20.h"
#include "esp_log.h"

static onewire_bus_handle_t bus_sonda;
static ds18b20_device_handle_t sonda;

static bool buscar(void)
{
    if (!bus_sonda) {
        return false;
    }
    onewire_device_iter_handle_t iter;
    if (onewire_new_device_iter(bus_sonda, &iter) != ESP_OK) {
        return false;
    }
    onewire_device_t dispositivo;
    while (onewire_device_iter_get_next(iter, &dispositivo) == ESP_OK) {
        ds18b20_config_t cfg = {};
        if (ds18b20_new_device_from_enumeration(&dispositivo, &cfg, &sonda) == ESP_OK) {
            break;
        }
    }
    onewire_del_device_iter(iter);
    if (!sonda) {
        return false;
    }
    if (ds18b20_set_resolution(sonda, DS18B20_RESOLUTION_9B) != ESP_OK) {
        ds18b20_del_device(sonda);
        sonda = NULL;
        return false;
    }
    return true;
}

esp_err_t hfr_sondas_iniciar(void)
{
    onewire_bus_config_t bus = {
        .bus_gpio_num = HFR_PIN_HELADERA,
        .flags.en_pull_up = true,
    };
    onewire_bus_rmt_config_t rmt = {.max_rx_bytes = 10};
    esp_err_t err = onewire_new_bus_rmt(&bus, &rmt, &bus_sonda);
    if (err != ESP_OK) {
        ESP_LOGE("SONDAS", "Bus de heladera: %s", esp_err_to_name(err));
    } else if (!buscar()) {
        ESP_LOGW("SONDAS", "Sin DS18B20 de heladera; se volvera a buscar al medir");
    }
    return err;
}

bool hfr_sonda_leer(float *temperatura)
{
    if (!bus_sonda || (!sonda && !buscar())) {
        return false;
    }
    esp_err_t err = ds18b20_trigger_temperature_conversion(sonda);
    if (err == ESP_OK) {
        err = ds18b20_get_temperature(sonda, temperatura);
    }
    if (err != ESP_OK) {
        ds18b20_del_device(sonda);
        sonda = NULL;
        return false;
    }
    return true;
}
