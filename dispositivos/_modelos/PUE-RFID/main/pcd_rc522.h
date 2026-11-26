#pragma once
/*
 * Driver minimo del RC522 (MFRC522) por SPI para la puerta y el lector de banco. Solo lo que hace
 * falta: reiniciar, ver si sigue vivo, prender/apagar la antena y mandar tramas CRUDAS. Todo lo
 * de la tarjeta (CRC, anticolision, ISO-DEP) esta en comun/ y se prueba en la compu.
 *
 * Reemplaza al componente abobija/rc522: ese comprueba que la tarjeta sigue ahi mandandole REQA, y
 * una NTAG 424 DNA ya seleccionada (SAK 0x20) no lo contesta, asi que lo daba por ido y la volvia a
 * leer una y otra vez con el llavero quieto.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Crea el bus SPI y el dispositivo (una sola vez). */
esp_err_t pcd_bus_iniciar(void);

/* Reinicio por hardware y por software, configura y prende la antena. ESP_OK si el chip contesta
   con una version de MFRC522 (0x91 o 0x92). */
esp_err_t pcd_reiniciar(void);

/* El chip contesta Y la antena esta prendida. Un RC522 que se reinicio solo (bajon de la
   alimentacion) sigue contestando la version pero tiene la antena apagada y no lee nada. */
bool pcd_vivo(uint8_t *version, uint8_t *tx_control);

/* Apaga el campo un rato: la tarjeta se reinicia (vuelve a IDLE y sale de ISO-DEP). */
void pcd_reiniciar_campo(void);

/* iso14443a_ops_t.txrx: trama cruda sin CRC. Devuelve bytes recibidos o negativo. */
int pcd_txrx(void *ctx, const uint8_t *tx, size_t ntx, uint8_t tx_bits, uint8_t *rx, size_t max);

/* isodep_t.trama: agrega el CRC_A a lo que manda y lo comprueba (y saca) de lo que recibe. */
int pcd_trama_iso(void *ctx, const uint8_t *in, size_t n, uint8_t *out, size_t max);
