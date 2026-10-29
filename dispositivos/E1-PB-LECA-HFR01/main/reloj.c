#include "reloj.h"
#include "bus_i2c.h"
#include "hardware.h"
#include "logica.h"
#include "esp_log.h"
#include <sys/time.h>
#include <time.h>

static i2c_master_dev_handle_t rtc;
static uint32_t ultima_escritura;

static bool leer(uint32_t *utc)
{
    uint8_t direccion = 0x0f;
    uint8_t estado;
    if (rtc == NULL || i2c_master_transmit_receive(rtc, &direccion, 1, &estado, 1, 100) != ESP_OK ||
        (estado & 0x80) != 0) {
        return false;
    }
    direccion = 0;
    uint8_t registros[7];
    if (i2c_master_transmit_receive(rtc, &direccion, 1, registros, 7, 100) != ESP_OK ||
        !hfr_rtc_decodificar(registros, utc)) {
        return false;
    }
    *utc += HFR_RTC_OFFSET_S;
    return true;
}

uint32_t hfr_reloj_utc(void)
{
    time_t ahora = time(NULL);
    if (ahora >= 1700000000 && ahora < 4102444800LL) {
        return (uint32_t)ahora;
    }
    uint32_t utc = 0;
    leer(&utc);
    return utc;
}

void hfr_reloj_iniciar(void)
{
    if (hfr_i2c_dispositivo(HFR_RTC_DIRECCION, &rtc) != ESP_OK) {
        ESP_LOGW("RELOJ", "DS3231 ausente; la mesh ajustara la hora");
        return;
    }
    uint32_t utc;
    if (leer(&utc)) {
        struct timeval hora = {.tv_sec = utc};
        settimeofday(&hora, NULL);
        ESP_LOGI("RELOJ", "Hora UTC recuperada del DS3231");
    } else {
        ESP_LOGW("RELOJ", "RTC sin hora valida; no se inventa una fecha");
    }
}

void hfr_reloj_mantener(bool mesh_conectada)
{
    uint32_t utc = hfr_reloj_utc();
    if (!mesh_conectada || rtc == NULL || utc == 0) {
        return;
    }
    uint32_t guardada;
    bool coincide = leer(&guardada) &&
                    (utc > guardada ? utc - guardada : guardada - utc) <= 2;
    if (coincide && ultima_escritura != 0 && utc - ultima_escritura < 3600) {
        return;
    }
    uint8_t datos[8] = {0};
    if (!hfr_rtc_codificar(utc - HFR_RTC_OFFSET_S, datos + 1) ||
        i2c_master_transmit(rtc, datos, sizeof(datos), 100) != ESP_OK) {
        return;
    }
    uint8_t direccion = 0x0f;
    uint8_t estado;
    if (i2c_master_transmit_receive(rtc, &direccion, 1, &estado, 1, 100) == ESP_OK) {
        uint8_t comando[2] = {0x0f, estado & ~0x80};
        if (i2c_master_transmit(rtc, comando, sizeof(comando), 100) == ESP_OK) {
            ultima_escritura = utc;
        }
    }
}
