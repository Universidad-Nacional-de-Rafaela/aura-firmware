#pragma once
/*
 * ISO/IEC 14443-3 tipo A del lado lector, sin I/O propio: CRC_A, REQA/WUPA, anticolision y select
 * en cascada (UID de 4, 7 y 10 bytes) y HLTA. El lector (RC522) pone la funcion `txrx`, que manda una
 * trama cruda y devuelve lo que contesto, tal cual: NO calcula ni comprueba el CRC; eso lo hace
 * este header con crc_a(). Asi el RC522 se usa con TxCRC/RxCRC apagados y esta logica se prueba en
 * la compu con una tarjeta simulada.
 *
 * Solo una tarjeta a la vez: si hay dos en el campo la anticolision falla (no se las separa).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ISO14443A_UID_MAX 10

typedef struct {
  // Manda `ntx` bytes (el ultimo con solo `tx_bits` bits validos; 0 = los 8) y deja en rx lo que
  // contesto la tarjeta. Devuelve la cantidad de bytes recibidos (los bits sueltos de un byte
  // incompleto cuentan como un byte), o un numero negativo si no contesto o hubo error de
  // paridad/colision/protocolo.
  int (*txrx)(void *ctx, const uint8_t *tx, size_t ntx, uint8_t tx_bits, uint8_t *rx, size_t max);
  void *ctx;
} iso14443a_ops_t;

typedef struct {
  uint8_t largo;                  // 4, 7 o 10
  uint8_t uid[ISO14443A_UID_MAX];
  uint8_t sak;                    // SAK del ultimo nivel (bit 0x20 = habla ISO-DEP)
  uint8_t atqa[2];
} iso14443a_tarjeta_t;

typedef enum {
  ISO14443A_OK = 0,
  ISO14443A_SIN_TARJETA,          // nadie contesta el REQA/WUPA
  ISO14443A_ERR_ANTICOLISION,     // contestaron dos, o la respuesta vino rota
  ISO14443A_ERR_BCC,
  ISO14443A_ERR_CRC,
  ISO14443A_ERR_SELECCION         // el select no devolvio un SAK valido
} iso14443a_resultado_t;

// CRC_A (CRC-16 con polinomio 0x8408 y valor inicial 0x6363). lo = byte que se manda primero.
static inline void iso14443a_crc(const uint8_t *d, size_t n, uint8_t out[2]) {
  uint16_t crc = 0x6363;
  for (size_t i = 0; i < n; i++) {
    uint8_t b = d[i] ^ (uint8_t)(crc & 0xFF);
    b = (uint8_t)(b ^ (b << 4));
    crc = (uint16_t)((crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ ((uint16_t)b >> 4));
  }
  out[0] = (uint8_t)(crc & 0xFF);
  out[1] = (uint8_t)(crc >> 8);
}

// true si los dos ultimos bytes de d[0..n) son el CRC_A de los anteriores.
static inline bool iso14443a_crc_ok(const uint8_t *d, size_t n) {
  if (n < 3) return false;
  uint8_t c[2];
  iso14443a_crc(d, n - 2, c);
  return c[0] == d[n - 2] && c[1] == d[n - 1];
}

// REQA (0x26) o WUPA (0x52): trama corta de 7 bits. El WUPA tambien despierta a la que esta en HALT.
static inline iso14443a_resultado_t iso14443a_despertar(const iso14443a_ops_t *o, bool wupa, uint8_t atqa[2]) {
  uint8_t cmd = wupa ? 0x52 : 0x26;
  uint8_t rx[4];
  int n = o->txrx(o->ctx, &cmd, 1, 7, rx, sizeof(rx));
  if (n < 0) return ISO14443A_SIN_TARJETA;
  if (n != 2) return ISO14443A_ERR_ANTICOLISION;
  atqa[0] = rx[0];
  atqa[1] = rx[1];
  return ISO14443A_OK;
}

// Anticolision y select en cascada de una tarjeta que ya contesto el REQA/WUPA.
static inline iso14443a_resultado_t iso14443a_seleccionar(const iso14443a_ops_t *o, iso14443a_tarjeta_t *t) {
  static const uint8_t nivel[3] = {0x93, 0x95, 0x97};
  uint8_t rx[8], tx[9];
  t->largo = 0;
  for (int i = 0; i < 3; i++) {
    // anticolision: SEL, NVB=0x20 (2 bytes validos) -> 4 bytes de UID + BCC
    tx[0] = nivel[i];
    tx[1] = 0x20;
    int n = o->txrx(o->ctx, tx, 2, 0, rx, sizeof(rx));
    if (n != 5) return ISO14443A_ERR_ANTICOLISION;
    if ((uint8_t)(rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4]) return ISO14443A_ERR_BCC;
    // select: SEL, NVB=0x70, UID(4), BCC, CRC
    uint8_t parte[5];
    memcpy(parte, rx, 5);
    tx[0] = nivel[i];
    tx[1] = 0x70;
    memcpy(&tx[2], parte, 5);
    iso14443a_crc(tx, 7, &tx[7]);
    n = o->txrx(o->ctx, tx, 9, 0, rx, sizeof(rx));
    if (n != 3) return ISO14443A_ERR_SELECCION;
    if (!iso14443a_crc_ok(rx, 3)) return ISO14443A_ERR_CRC;
    uint8_t sak = rx[0];
    bool cascada = parte[0] == 0x88;
    if (cascada != ((sak & 0x04) != 0)) return ISO14443A_ERR_SELECCION;  // el SAK tiene que decir lo mismo que el CT
    if (cascada) {
      if (t->largo + 3 > ISO14443A_UID_MAX) return ISO14443A_ERR_SELECCION;
      memcpy(&t->uid[t->largo], &parte[1], 3);
      t->largo = (uint8_t)(t->largo + 3);
      continue;
    }
    if (t->largo + 4 > ISO14443A_UID_MAX) return ISO14443A_ERR_SELECCION;
    memcpy(&t->uid[t->largo], parte, 4);
    t->largo = (uint8_t)(t->largo + 4);
    t->sak = sak;
    return ISO14443A_OK;
  }
  return ISO14443A_ERR_SELECCION;
}

// HLTA: manda la tarjeta a HALT (solo contesta a un WUPA). La tarjeta no contesta nada.
static inline void iso14443a_halt(const iso14443a_ops_t *o) {
  uint8_t tx[4] = {0x50, 0x00, 0, 0}, rx[4];
  iso14443a_crc(tx, 2, &tx[2]);
  (void)o->txrx(o->ctx, tx, 4, 0, rx, sizeof(rx));
}

#ifdef __cplusplus
}
#endif
