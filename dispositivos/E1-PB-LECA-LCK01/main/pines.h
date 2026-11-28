#pragma once
/*
 * Pines de la ESP32-C3 SuperMini para los 4 lockers del gabinete. Conexionado completo en la ficha
 * (README.md, "Hardware y conexionado").
 *
 * PROPUESTA, a confirmar con @miltonpozzo. Lo que tiene que cumplir: ninguna cerradura se
 * destraba mientras la placa arranca, ni con la placa sin firmware. Por eso:
 *  - los gates van en pines sin funcion de arranque (no GPIO2, 8 ni 9) ni salida de la UART del
 *    bootloader (no GPIO21, que transmite al arrancar), y cada gate tiene su pull-down de 10 kohm
 *    a GND: mientras el pin esta en alta impedancia (arranque o placa sin firmware) el MOSFET
 *    queda apagado;
 *  - firmware: salida_cerradura_reposo_inicial() es lo PRIMERO de app_main y escribe 0 en cada pin
 *    antes de pasarlo a salida.
 * Medir con el tester, en cada gate, que queda en 0 V durante el arranque y con la placa borrada.
 */
#include "driver/gpio.h"
#include "locker_logica.h"

#if !CONFIG_IDF_TARGET_ESP32C3
#error "Esta hoja es para la ESP32-C3 SuperMini: idf.py set-target esp32c3"
#endif

/* Gate del IRLZ44N de cada locker, por 470 ohm, con 10 kohm de gate a GND. 1 = energizada. */
static const gpio_num_t PIN_CERRADURA[CANT_LOCKERS] = {GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_3, GPIO_NUM_10};

/* Microswitch de cada locker: COM a GND, NO al pin, con el pull-up interno. Cerrado = switch
   apretado = pin en 0. Un cable cortado o un switch sin conectar se lee como 1 = abierto. */
static const gpio_num_t PIN_MICROSWITCH[CANT_LOCKERS] = {GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_4};

/* LED azul de la SuperMini, activo en bajo */
#define PIN_LED GPIO_NUM_8
