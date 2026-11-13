#pragma once
// BH1750 (sensor de luz I2C) para ESP-IDF puro, sobre el driver i2c_master.
// Puerto de la libreria BH1750 de Christopher Laws: solo el modo continuo de
// alta resolucion, que es lo que usa este dispositivo.
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  i2c_master_dev_handle_t dev;
  bool ok;
} bh1750_t;

// Comandos del BH1750.
#define BH1750_POWER_ON      0x01
#define BH1750_CONT_H_RES    0x10   // modo continuo, alta resolucion (1 lx)

// Agrega el dispositivo al bus ya creado y lo pone en modo continuo. false si
// no contesta.
bool bh1750_init(bh1750_t* s, i2c_master_bus_handle_t bus, uint8_t addr);

// Lee el ultimo valor del modo continuo. Devuelve < 0 si falla (y marca !ok).
float bh1750_leer_lux(bh1750_t* s);

#ifdef __cplusplus
}
#endif
