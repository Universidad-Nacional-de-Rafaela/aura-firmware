/* Sensor de luz ambiental BH1750 (I2C 0x23, SDA=GPIO6, SCL=GPIO7), 1 a 65535 lx */
#pragma once
#include <stdbool.h>

/* Lee la iluminancia en lux. Inicializa el bus y configura el sensor la primera vez y
   reintenta si fallo antes, asi que sirve conectarlo despues de arrancar. false = sin dato valido. */
bool luz_leer(double *lux);

/* Banco: imprime las direcciones que responden en el bus. */
void luz_escanear(void);
