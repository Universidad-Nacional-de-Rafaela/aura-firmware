#include "bh1750.h"
#include "esp_log.h"
#include "aura_tiempo.h"

#define I2C_TIMEOUT_MS 50

bool bh1750_init(bh1750_t* s, i2c_master_bus_handle_t bus, uint8_t addr) {
  s->ok = false;
  i2c_device_config_t cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address  = addr,
      .scl_speed_hz    = 100000,
  };
  if (i2c_master_bus_add_device(bus, &cfg, &s->dev) != ESP_OK) return false;

  uint8_t on = BH1750_POWER_ON;
  if (i2c_master_transmit(s->dev, &on, 1, I2C_TIMEOUT_MS) != ESP_OK) return false;
  uint8_t cont = BH1750_CONT_H_RES;
  if (i2c_master_transmit(s->dev, &cont, 1, I2C_TIMEOUT_MS) != ESP_OK) return false;
  s->ok = true;
  return true;
}

float bh1750_leer_lux(bh1750_t* s) {
  if (!s->ok) return -1.0f;
  uint8_t buf[2];
  if (i2c_master_receive(s->dev, buf, 2, I2C_TIMEOUT_MS) != ESP_OK) {
    s->ok = false;
    return -1.0f;
  }
  uint16_t raw = ((uint16_t)buf[0] << 8) | buf[1];
  return (float)raw / 1.2f;
}
