#include <stdio.h>
#include <math.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "luz.h"

static const char *TAG = "luz";

#define PIN_SDA            GPIO_NUM_6
#define PIN_SCL            GPIO_NUM_7
#define BH1750_ADDR        0x23         /* ADDR a GND o sin conectar; 0x5C si ADDR esta a VCC */
#define BH1750_POWER_ON    0x01
#define BH1750_CONT_HRES   0x10         /* continuo, alta resolucion (1 lx, ~120 ms) */
#define CALENTAMIENTO_US   200000       /* primera conversion */

static i2c_master_bus_handle_t bus = NULL;
static i2c_master_dev_handle_t dev = NULL;
static bool configurado = false;
static int64_t t_config_us = 0;

static esp_err_t asegurar_bus(void)
{
    if (bus) return ESP_OK;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) bus = NULL;
    return err;
}

static esp_err_t asegurar_sensor(void)
{
    esp_err_t err = asegurar_bus();
    if (err != ESP_OK) return err;
    if (!dev) {
        i2c_device_config_t d = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BH1750_ADDR,
            .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(bus, &d, &dev);
        if (err != ESP_OK) { dev = NULL; return err; }
    }
    if (!configurado) {
        uint8_t cmd = BH1750_POWER_ON;
        err = i2c_master_transmit(dev, &cmd, 1, 100);
        if (err != ESP_OK) return err;
        cmd = BH1750_CONT_HRES;
        err = i2c_master_transmit(dev, &cmd, 1, 100);
        if (err != ESP_OK) return err;
        configurado = true;
        t_config_us = esp_timer_get_time();
        ESP_LOGI(TAG, "BH1750 configurado en 0x%02X", BH1750_ADDR);
    }
    return ESP_OK;
}

bool luz_leer(double *lux)
{
    if (asegurar_sensor() != ESP_OK) return false;
    if (esp_timer_get_time() - t_config_us < CALENTAMIENTO_US) return false;

    uint8_t d[2] = {0};
    if (i2c_master_receive(dev, d, 2, 100) != ESP_OK) {
        configurado = false;        /* se reconfigura si el sensor se reinicio o se desconecto */
        return false;
    }
    uint16_t raw = ((uint16_t)d[0] << 8) | d[1];
    *lux = round((double)raw / 1.2 * 10.0) / 10.0;     /* datasheet: lux = raw / 1.2 */
    return true;
}

void luz_escanear(void)
{
    if (asegurar_bus() != ESP_OK) { printf("No se pudo crear el bus I2C\n"); return; }
    int n = 0;
    for (int a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            printf("  responde 0x%02X%s\n", a, a == BH1750_ADDR ? "  (BH1750)" : "");
            n++;
        }
    }
    printf("%d dispositivo(s) en el bus (SDA=GPIO%d, SCL=GPIO%d)\n", n, PIN_SDA, PIN_SCL);
    if (n == 0) printf("Revisar conexiones, 3V3/GND y los pull-ups de SDA/SCL (4.7k a 3V3)\n");
}
