#pragma once
/*
 * Operaciones de alto nivel sobre una NTAG 424 DNA, sin I/O propio: habla con la tarjeta por una
 * funcion que pasa quien llama (ntag_ops_t). Lo usan la puerta (autenticar) y la estacion de
 * enrolamiento (autenticar y cambiar la clave 0). Se prueba en la compu con una tarjeta simulada.
 *
 * Una sesion (ev2_sesion) vale hasta que la tarjeta la cierra: un fallo de autenticacion, un
 * cambio de la clave con la que se autentico, o sacar la tarjeta del campo.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "ev2_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NTAG_UID_BYTES 7

typedef struct {
  // Manda un APDU a la tarjeta y deja su respuesta (con SW1 SW2 al final) en resp. Devuelve el
  // largo de la respuesta, o un numero negativo si la tarjeta no contesto.
  int (*apdu)(void *ctx, const uint8_t *cmd, size_t n, uint8_t *resp, size_t max);
  // Clave AES-128 de la tarjeta con ese UID (HMAC con la clave maestra; en la placa lo calcula el
  // eFuse). false si no se pudo.
  bool (*clave_de_tarjeta)(void *ctx, const uint8_t *uid, size_t largo, uint8_t out[16]);
  // 16 bytes de azar criptografico. false si no hay.
  bool (*azar)(void *ctx, uint8_t out[16]);
  void *ctx;
} ntag_ops_t;

typedef enum {
  NTAG_OK = 0,
  NTAG_ERR_AZAR,           // no hubo azar: no se le hablo a la tarjeta
  NTAG_ERR_COMUNICACION,   // la tarjeta no contesto
  NTAG_ERR_SELECCION,      // no tiene la aplicacion de la NTAG 424 DNA
  NTAG_CLAVE_RECHAZADA,    // la tarjeta dijo que esa clave no es la suya (91 AE)
  NTAG_ERR_PROTOCOLO,      // contesto algo que una tarjeta de verdad no contesta
  NTAG_UID_NO_COINCIDE     // su UID real no es el esperado
} ntag_resultado_t;

// Borra de verdad (el compilador no puede descartar el borrado de algo que no se vuelve a leer).
static inline void ntag_borrar(void *p, size_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;
  while (n--) *v++ = 0;
}

static inline int ntag_sw_es(const uint8_t *resp, int n, uint8_t sw1, uint8_t sw2) {
  return n >= 2 && resp[n - 2] == sw1 && resp[n - 1] == sw2;
}

// SELECT de la aplicacion (DF name D2760000850101) y AuthenticateEV2First con la clave 0.
// Deja en *s las claves de sesion si sale NTAG_OK.
static inline ntag_resultado_t ntag_autenticar(const ntag_ops_t *ops, const uint8_t clave[16], ev2_sesion *s) {
  static const uint8_t seleccionar[] = {0x00, 0xA4, 0x04, 0x0C, 0x07, 0xD2, 0x76,
                                        0x00, 0x00, 0x85, 0x01, 0x01, 0x00};
  uint8_t rnd_a[16], resp[64], ap1[8], ap2[38];
  size_t l2 = 0;
  ntag_resultado_t res = NTAG_ERR_PROTOCOLO;
  int n;

  memset(s, 0, sizeof(*s));
  if (!ops->azar(ops->ctx, rnd_a)) return NTAG_ERR_AZAR;

  n = ops->apdu(ops->ctx, seleccionar, sizeof(seleccionar), resp, sizeof(resp));
  if (n < 0) { res = NTAG_ERR_COMUNICACION; goto fin; }
  if (!ntag_sw_es(resp, n, 0x90, 0x00)) { res = NTAG_ERR_SELECCION; goto fin; }

  ev2_parte1_apdu(s, clave, rnd_a, 0, ap1);
  n = ops->apdu(ops->ctx, ap1, sizeof(ap1), resp, sizeof(resp));
  if (n < 0) { res = NTAG_ERR_COMUNICACION; goto fin; }
  if (ev2_parte2_apdu(s, resp, (size_t)n, ap2, &l2) != EV2_OK) { res = NTAG_ERR_PROTOCOLO; goto fin; }
  n = ops->apdu(ops->ctx, ap2, l2, resp, sizeof(resp));
  if (n < 0) { res = NTAG_ERR_COMUNICACION; goto fin; }
  if (ntag_sw_es(resp, n, 0x91, 0xAE)) { res = NTAG_CLAVE_RECHAZADA; goto fin; }
  if (ev2_parte2_resp(s, resp, (size_t)n) != EV2_OK) { res = NTAG_ERR_PROTOCOLO; goto fin; }
  res = NTAG_OK;

fin:
  ntag_borrar(rnd_a, sizeof(rnd_a));
  if (res != NTAG_OK) ntag_borrar(s, sizeof(*s));
  return res;
}

// GetCardUID por el canal seguro (CommMode Full): el UID real de la tarjeta, no el del aire.
static inline ntag_resultado_t ntag_leer_uid(const ntag_ops_t *ops, ev2_sesion *s, uint8_t uid[NTAG_UID_BYTES]) {
  uint8_t ap[48], resp[64], dat[64];
  size_t nd = 0;
  size_t l = ev2_apdu_mac(s, 0x51, NULL, 0, NULL, 0, ap);
  int n = ops->apdu(ops->ctx, ap, l, resp, sizeof(resp));
  if (n < 0) return NTAG_ERR_COMUNICACION;
  if (ev2_resp_verificar(s, resp, (size_t)n, &nd) != EV2_OK) return NTAG_ERR_PROTOCOLO;
  if (nd < 16 || nd % 16 != 0 || nd > sizeof(dat)) return NTAG_ERR_PROTOCOLO;
  ev2_resp_descifrar(s, resp, dat, nd);
  memcpy(uid, dat, NTAG_UID_BYTES);
  ntag_borrar(dat, sizeof(dat));
  return NTAG_OK;
}

// Lo mismo, pero compara con el UID esperado sin cortar en el primer byte distinto.
static inline ntag_resultado_t ntag_confirmar_uid(const ntag_ops_t *ops, ev2_sesion *s, const uint8_t *esperado) {
  uint8_t real[NTAG_UID_BYTES];
  ntag_resultado_t r = ntag_leer_uid(ops, s, real);
  if (r != NTAG_OK) return r;
  uint8_t dif = 0;
  for (int i = 0; i < NTAG_UID_BYTES; i++) dif |= (uint8_t)(real[i] ^ esperado[i]);
  return dif ? NTAG_UID_NO_COINCIDE : NTAG_OK;
}

// ChangeKey de la clave 0 con la sesion abierta con esa misma clave. Al cambiarla la tarjeta
// cierra la sesion, asi que la respuesta es solo "91 00" y la sesion queda inutilizada. Que el
// cambio se aplico se comprueba autenticando con la clave nueva: NO alcanza con el "91 00".
//   NTAG_OK              la tarjeta dijo que si
//   NTAG_ERR_COMUNICACION no se sabe si lo aplico
//   NTAG_ERR_PROTOCOLO   la tarjeta lo rechazo (SW distinto de 91 00)
static inline ntag_resultado_t ntag_cambiar_clave0(const ntag_ops_t *ops, ev2_sesion *s,
                                                   const uint8_t nueva[16], uint8_t version) {
  uint8_t ap[64], resp[64];
  size_t l = ev2_apdu_cambiar_clave0(s, nueva, version, ap);
  int n = ops->apdu(ops->ctx, ap, l, resp, sizeof(resp));
  ntag_borrar(ap, sizeof(ap));
  ntag_borrar(s, sizeof(*s));
  if (n < 0) return NTAG_ERR_COMUNICACION;
  return ntag_sw_es(resp, n, 0x91, 0x00) ? NTAG_OK : NTAG_ERR_PROTOCOLO;
}

#ifdef __cplusplus
}
#endif
