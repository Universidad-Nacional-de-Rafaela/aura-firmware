#pragma once
// SHT30/SHT31 (temperatura y humedad I2C) para ESP-IDF puro, sobre i2c_master.
// Modo single-shot, alta repetibilidad, sin clock stretching (0x2400).
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  i2c_master_dev_handle_t dev;
  bool ok;
} sht31_t;

// Agrega el dispositivo al bus. false si no contesta.
bool sht31_init(sht31_t* s, i2c_master_bus_handle_t bus, uint8_t addr);

// Lee temperatura (°C) y humedad (%). false si no contesta o el CRC no cierra.
bool sht31_leer(sht31_t* s, float* temp_c, float* humedad_pct);

#ifdef __cplusplus
}
#endif
