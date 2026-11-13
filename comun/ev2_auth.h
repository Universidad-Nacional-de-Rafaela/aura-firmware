#pragma once
/*
 * AuthenticateEV2First (NTAG 424 DNA) del lado lector, sin I/O: arma los APDU,
 * interpreta las respuestas y deriva las claves de sesion. La tarjeta se habla
 * por afuera (ISO-DEP). El azar (RndA) lo pone quien llama.
 *
 *   Parte 1:  90 71 00 00 02 <KeyNo> 00 00        -> E(K, RndB) || 91 AF
 *   Parte 2:  90 AF 00 00 20 E(K, RndA || RndB')  -> E(K, TI || RndA' || PDcap2 || PCDcap2) || 91 00
 *   (RndB' y RndA' son RndB y RndA rotados un byte a la izquierda; CBC con IV en cero.)
 */
#include "aes_cmac.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  EV2_OK = 0,
  EV2_ERR_LARGO,       // la respuesta no tiene el largo esperado
  EV2_ERR_ESTADO,      // SW distinto del esperado (la tarjeta rechazo)
  EV2_ERR_RNDA,        // la tarjeta no demostro conocer la clave (RndA' no coincide)
  EV2_ERR_SECUENCIA,   // se llamo fuera de orden
  EV2_ERR_MAC          // el MAC de la respuesta no coincide
} ev2_resultado;

typedef struct {
  uint8_t clave[16];
  uint8_t rnd_a[16];
  uint8_t rnd_b[16];
  int paso;  // 0 nada, 1 parte 1 armada, 2 parte 1 resuelta, 3 autenticado
  // Resultado
  uint8_t ti[4];
  uint8_t pdcap2[6];
  uint8_t pcdcap2[6];
  uint8_t k_ses_enc[16];
  uint8_t k_ses_mac[16];
  uint16_t cmd_ctr;  // contador de comandos de la sesion (arranca en 0)
} ev2_sesion;

static inline void ev2_rotar_izq(const uint8_t in[16], uint8_t out[16]) {
  uint8_t p = in[0];
  for (int i = 0; i < 15; i++) out[i] = in[i + 1];
  out[15] = p;
}

// Devuelve el largo del APDU (8).
static inline size_t ev2_parte1_apdu(ev2_sesion *s, const uint8_t clave[16],
                                     const uint8_t rnd_a[16], uint8_t key_no, uint8_t out[8]) {
  memset(s, 0, sizeof(*s));
  memcpy(s->clave, clave, 16);
  memcpy(s->rnd_a, rnd_a, 16);
  const uint8_t apdu[8] = {0x90, 0x71, 0x00, 0x00, 0x02, key_no, 0x00, 0x00};
  memcpy(out, apdu, 8);
  s->paso = 1;
  return 8;
}

static inline int ev2_sw_es(const uint8_t *resp, size_t n, uint8_t sw1, uint8_t sw2) {
  return n >= 2 && resp[n - 2] == sw1 && resp[n - 1] == sw2;
}

// resp = 16 bytes + SW. Arma el APDU de la parte 2 (37 bytes) en out.
static inline ev2_resultado ev2_parte2_apdu(ev2_sesion *s, const uint8_t *resp, size_t n, uint8_t out[38],
                                            size_t *largo) {
  if (s->paso != 1) return EV2_ERR_SECUENCIA;
  if (n != 18) return EV2_ERR_LARGO;
  if (!ev2_sw_es(resp, n, 0x91, 0xAF)) return EV2_ERR_ESTADO;

  aes128_ctx c;
  aes128_iniciar(&c, s->clave);
  const uint8_t iv[16] = {0};
  aes128_cbc_descifrar(&c, iv, resp, s->rnd_b, 16);

  uint8_t m[32];
  memcpy(m, s->rnd_a, 16);
  ev2_rotar_izq(s->rnd_b, &m[16]);
  uint8_t cifrado[32];
  aes128_cbc_cifrar(&c, iv, m, cifrado, 32);

  const uint8_t cab[5] = {0x90, 0xAF, 0x00, 0x00, 0x20};
  memcpy(out, cab, 5);
  memcpy(&out[5], cifrado, 32);
  out[37] = 0x00;
  *largo = 38;
  s->paso = 2;
  return EV2_OK;
}

static inline void ev2_vector_sesion(const ev2_sesion *s, uint8_t cab0, uint8_t cab1, uint8_t sv[32]) {
  sv[0] = cab0;
  sv[1] = cab1;
  sv[2] = 0x00;
  sv[3] = 0x01;
  sv[4] = 0x00;
  sv[5] = 0x80;
  sv[6] = s->rnd_a[0];
  sv[7] = s->rnd_a[1];
  for (int i = 0; i < 6; i++) sv[8 + i] = (uint8_t)(s->rnd_a[2 + i] ^ s->rnd_b[i]);
  memcpy(&sv[14], &s->rnd_b[6], 10);
  memcpy(&sv[24], &s->rnd_a[8], 8);
}

// resp = 32 bytes + SW. Verifica que la tarjeta conoce la clave y deriva las claves de sesion.
static inline ev2_resultado ev2_parte2_resp(ev2_sesion *s, const uint8_t *resp, size_t n) {
  if (s->paso != 2) return EV2_ERR_SECUENCIA;
  if (n != 34) return EV2_ERR_LARGO;
  if (!ev2_sw_es(resp, n, 0x91, 0x00)) return EV2_ERR_ESTADO;

  aes128_ctx c;
  aes128_iniciar(&c, s->clave);
  const uint8_t iv[16] = {0};
  uint8_t d[32];
  aes128_cbc_descifrar(&c, iv, resp, d, 32);

  uint8_t esperado[16];
  ev2_rotar_izq(s->rnd_a, esperado);
  // comparacion sin cortar en el primer byte distinto
  uint8_t dif = 0;
  for (int i = 0; i < 16; i++) dif |= (uint8_t)(d[4 + i] ^ esperado[i]);
  if (dif) return EV2_ERR_RNDA;

  memcpy(s->ti, d, 4);
  memcpy(s->pdcap2, &d[20], 6);
  memcpy(s->pcdcap2, &d[26], 6);

  uint8_t sv[32];
  ev2_vector_sesion(s, 0xA5, 0x5A, sv);
  aes_cmac(s->clave, sv, 32, s->k_ses_enc);
  ev2_vector_sesion(s, 0x5A, 0xA5, sv);
  aes_cmac(s->clave, sv, 32, s->k_ses_mac);
  s->paso = 3;
  return EV2_OK;
}


// ---- Mensajeria segura EV2 (CommMode MAC / Full) ----

// El MAC que viaja son los bytes de indice impar del CMAC de 16.
static inline void ev2_truncar(const uint8_t m[16], uint8_t out[8]) {
  for (int i = 0; i < 8; i++) out[i] = m[2 * i + 1];
}

// IV = E(KSesAuthENC, cab0 cab1 || TI || CmdCtr(LE) || 00*8). Comando: A5 5A. Respuesta: 5A A5.
static inline void ev2_iv(const ev2_sesion *s, uint8_t cab0, uint8_t cab1, uint8_t iv[16]) {
  uint8_t b[16] = {0};
  b[0] = cab0;
  b[1] = cab1;
  memcpy(&b[2], s->ti, 4);
  b[6] = (uint8_t)(s->cmd_ctr & 0xFF);
  b[7] = (uint8_t)(s->cmd_ctr >> 8);
  aes128_ctx c;
  aes128_iniciar(&c, s->k_ses_enc);
  aes128_cifrar_bloque(&c, b, iv);
}

// MAC de comando: CMAC(KSesAuthMAC, ins || CmdCtr || TI || cab || datos) truncado.
static inline void ev2_mac_comando(const ev2_sesion *s, uint8_t ins, const uint8_t *cab, size_t ncab,
                                   const uint8_t *datos, size_t ndatos, uint8_t mac8[8]) {
  uint8_t m[80];
  size_t n = 0;
  m[n++] = ins;
  m[n++] = (uint8_t)(s->cmd_ctr & 0xFF);
  m[n++] = (uint8_t)(s->cmd_ctr >> 8);
  memcpy(&m[n], s->ti, 4);
  n += 4;
  if (ncab) { memcpy(&m[n], cab, ncab); n += ncab; }
  if (ndatos) { memcpy(&m[n], datos, ndatos); n += ndatos; }
  uint8_t t[16];
  aes_cmac(s->k_ses_mac, m, n, t);
  ev2_truncar(t, mac8);
}

// Arma 90 ins 00 00 Lc || cab || datos || MAC8 || 00. Devuelve el largo. Max: ncab+ndatos <= 40.
static inline size_t ev2_apdu_mac(const ev2_sesion *s, uint8_t ins, const uint8_t *cab, size_t ncab,
                                  const uint8_t *datos, size_t ndatos, uint8_t *out) {
  size_t n = 0;
  out[n++] = 0x90;
  out[n++] = ins;
  out[n++] = 0x00;
  out[n++] = 0x00;
  out[n++] = (uint8_t)(ncab + ndatos + 8);
  if (ncab) { memcpy(&out[n], cab, ncab); n += ncab; }
  if (ndatos) { memcpy(&out[n], datos, ndatos); n += ndatos; }
  ev2_mac_comando(s, ins, cab, ncab, datos, ndatos, &out[n]);
  n += 8;
  out[n++] = 0x00;
  return n;
}

// resp = datos || MAC8 || 91 00. Verifica el MAC con CmdCtr+1 y, si esta bien, avanza el contador.
// *ndatos recibe el largo de los datos (que quedan en resp[0..ndatos)).
static inline ev2_resultado ev2_resp_verificar(ev2_sesion *s, const uint8_t *resp, size_t n, size_t *ndatos) {
  if (s->paso != 3) return EV2_ERR_SECUENCIA;
  if (n < 10) return EV2_ERR_LARGO;
  if (!ev2_sw_es(resp, n, 0x91, 0x00)) return EV2_ERR_ESTADO;
  size_t nd = n - 10;
  if (nd > 64) return EV2_ERR_LARGO;
  uint16_t ctr = (uint16_t)(s->cmd_ctr + 1);
  uint8_t m[80];
  size_t k = 0;
  m[k++] = 0x00;
  m[k++] = (uint8_t)(ctr & 0xFF);
  m[k++] = (uint8_t)(ctr >> 8);
  memcpy(&m[k], s->ti, 4);
  k += 4;
  memcpy(&m[k], resp, nd);
  k += nd;
  uint8_t t[16], mac[8];
  aes_cmac(s->k_ses_mac, m, k, t);
  ev2_truncar(t, mac);
  uint8_t dif = 0;
  for (int i = 0; i < 8; i++) dif |= (uint8_t)(mac[i] ^ resp[nd + i]);
  if (dif) return EV2_ERR_MAC;
  s->cmd_ctr = ctr;
  *ndatos = nd;
  return EV2_OK;
}

// Descifra datos de respuesta (CommMode Full) con el contador ya avanzado. n multiplo de 16.
static inline void ev2_resp_descifrar(const ev2_sesion *s, const uint8_t *in, uint8_t *out, size_t n) {
  uint8_t iv[16];
  ev2_iv(s, 0x5A, 0xA5, iv);
  aes128_ctx c;
  aes128_iniciar(&c, s->k_ses_enc);
  aes128_cbc_descifrar(&c, iv, in, out, n);
}


// ChangeKey de la clave 0 (CommMode Full). Datos = E(KSesAuthENC, IV_cmd, nueva(16) || version(1) || 80 || 00*14).
// Cuando el cambio es de la clave con la que se autentico la sesion, la tarjeta cierra la sesion
// y hay que volver a autenticar con la clave nueva. La respuesta trae solo el MAC (0 bytes de datos).
// Devuelve el largo del APDU (5 + 1 + 32 + 8 + 1 = 47).
static inline size_t ev2_apdu_cambiar_clave0(const ev2_sesion *s, const uint8_t nueva[16], uint8_t version,
                                             uint8_t *out) {
  uint8_t plano[32] = {0};
  memcpy(plano, nueva, 16);
  plano[16] = version;
  plano[17] = 0x80;
  uint8_t iv[16], cif[32];
  ev2_iv(s, 0xA5, 0x5A, iv);
  aes128_ctx c;
  aes128_iniciar(&c, s->k_ses_enc);
  aes128_cbc_cifrar(&c, iv, plano, cif, 32);
  const uint8_t kn = 0x00;
  return ev2_apdu_mac(s, 0xC4, &kn, 1, cif, 32, out);
}

#ifdef __cplusplus
}
#endif
