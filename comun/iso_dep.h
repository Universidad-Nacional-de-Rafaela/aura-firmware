#pragma once
/*
 * Encuadre ISO/IEC 14443-4 (ISO-DEP) sobre una funcion de tramas, sin I/O propio: RATS, bloques I,
 * extension de tiempo (S(WTX)) y respuestas encadenadas. Lo usan la estacion de enrolamiento y la
 * puerta; cada una pone la funcion `trama` con su lector (RC522).
 *
 * La funcion `trama` manda PCB + datos y devuelve lo que contesto la tarjeta, las dos veces SIN los
 * dos bytes de CRC_A: calcular el CRC al mandar y comprobarlo al recibir es cosa del lector.
 *
 * No se encadena lo que se manda (un APDU tiene que entrar en una trama); lo que se recibe si.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ISODEP_MAX_TRAMA 64   // FIFO del RC522; el RATS pide FSD = 64
#define ISODEP_MAX_WTX 8

typedef struct {
  // Manda una trama (PCB + datos, sin CRC). Devuelve el largo de la respuesta (sin CRC), o un
  // numero negativo si la tarjeta no contesto o el CRC vino mal.
  int (*trama)(void *ctx, const uint8_t *tx, size_t n, uint8_t *rx, size_t max);
  void *ctx;
  uint8_t bloque;   // numero de bloque que toca mandar (0 o 1)
  uint8_t fsc;      // tamano maximo de trama que acepta la tarjeta (incluye PCB y CRC)
} isodep_t;

static inline uint8_t isodep_fsc_de_fsci(uint8_t fsci) {
  static const uint8_t tabla[] = {16, 24, 32, 40, 48, 64, 96, 128};
  return fsci < sizeof(tabla) ? tabla[fsci] : 128;
}

// RATS con FSDI = 5 (64 bytes) y CID = 0. true si la tarjeta contesto con una ATS valida.
static inline bool isodep_rats(isodep_t *d) {
  uint8_t rx[ISODEP_MAX_TRAMA];
  const uint8_t cmd[2] = {0xE0, 0x50};
  int n = d->trama(d->ctx, cmd, sizeof(cmd), rx, sizeof(rx));
  d->bloque = 0;
  d->fsc = 32;  // lo seguro si la ATS no lo dice
  if (n < 1 || rx[0] != (uint8_t)n) return false;       // el primer byte es el largo de la ATS
  if (n >= 2) d->fsc = isodep_fsc_de_fsci(rx[1] & 0x0F);
  return true;
}

// Manda un APDU dentro de un bloque I y deja la respuesta (con SW1 SW2) en resp. Devuelve su
// largo o un numero negativo.
static inline int isodep_apdu(isodep_t *d, const uint8_t *cmd, size_t n, uint8_t *resp, size_t max) {
  uint8_t tx[ISODEP_MAX_TRAMA], rx[ISODEP_MAX_TRAMA];
  size_t total = 0;
  int wtx = 0;
  if (n + 3 > d->fsc || n + 1 > sizeof(tx)) return -1;   // no entra en una trama
  tx[0] = (uint8_t)(0x02 | d->bloque);
  memcpy(&tx[1], cmd, n);
  size_t ntx = n + 1;
  for (;;) {
    int r = d->trama(d->ctx, tx, ntx, rx, sizeof(rx));
    if (r < 1) return -1;
    uint8_t pcb = rx[0];
    if ((pcb & 0xF7) == 0xF2) {                         // S(WTX): pide mas tiempo y se lo da
      if (r < 2 || ++wtx > ISODEP_MAX_WTX) return -1;
      tx[0] = 0xF2;
      tx[1] = rx[1];
      ntx = 2;
      continue;
    }
    if ((pcb & 0xC0) != 0x00) return -1;                // no es un bloque I
    if ((pcb & 0x01) != d->bloque) return -1;           // bloque que no toca
    size_t util = (size_t)r - 1;
    if (total + util > max) return -1;
    memcpy(&resp[total], &rx[1], util);
    total += util;
    d->bloque ^= 1;
    if (pcb & 0x10) {                                   // encadenado: pide el resto con R(ACK)
      tx[0] = (uint8_t)(0xA2 | d->bloque);
      ntx = 1;
      continue;
    }
    return (int)total;
  }
}

#ifdef __cplusplus
}
#endif
