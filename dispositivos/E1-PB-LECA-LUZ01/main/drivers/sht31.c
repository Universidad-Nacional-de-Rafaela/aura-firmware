#include "sht31.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "aura_tiempo.h"

#define I2C_TIMEOUT_MS 50
#define SHT31_CMD_MEAS 0x2400   // single shot, high repeatability, sin clock stretching

// CRC-8 del SHT3x: polinomio 0x31, init 0xFF (datasheet).
static uint8_t sht31_crc8(const uint8_t* data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

bool sht31_init(sht31_t* s, i2c_master_bus_handle_t bus, uint8_t addr) {
  s->ok = false;
  i2c_device_config_t cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address  = addr,
      .scl_speed_hz    = 100000,
  };
  if (i2c_master_bus_add_device(bus, &cfg, &s->dev) != ESP_OK) return false;

  uint8_t cmd[2] = {SHT31_CMD_MEAS >> 8, SHT31_CMD_MEAS & 0xFF};
  if (i2c_master_transmit(s->dev, cmd, 2, I2C_TIMEOUT_MS) != ESP_OK) return false;
  vTaskDelay(pdMS_TO_TICKS(20));
  s->ok = true;
  return true;
}

bool sht31_leer(sht31_t* s, float* temp_c, float* humedad_pct) {
  if (!s->ok) return false;
  uint8_t cmd[2] = {SHT31_CMD_MEAS >> 8, SHT31_CMD_MEAS & 0xFF};
  if (i2c_master_transmit(s->dev, cmd, 2, I2C_TIMEOUT_MS) != ESP_OK) {
    s->ok = false;
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(20));
  uint8_t buf[6];
  if (i2c_master_receive(s->dev, buf, 6, I2C_TIMEOUT_MS) != ESP_OK) {
    s->ok = false;
    return false;
  }
  if (sht31_crc8(&buf[0], 2) != buf[2] || sht31_crc8(&buf[3], 2) != buf[5]) {
    s->ok = false;
    return false;
  }
  uint16_t raw_t = ((uint16_t)buf[0] << 8) | buf[1];
  uint16_t raw_h = ((uint16_t)buf[3] << 8) | buf[4];
  *temp_c      = -45.0f + 175.0f * (float)raw_t / 65535.0f;
  *humedad_pct = 100.0f * (float)raw_h / 65535.0f;
  return true;
}
