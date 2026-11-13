#pragma once
/*
 * AES-128 (cifrar/descifrar), CBC con IV en cero y AES-CMAC (RFC 4493) en C puro.
 * Sin dependencias: se prueba en host. La S-box se calcula (inverso en GF(2^8) +
 * transformacion afin) para no depender de una tabla tipeada a mano.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t rk[176];  // 11 claves de ronda de 16 bytes
} aes128_ctx;

static uint8_t aes_sbox[256];
static uint8_t aes_inv_sbox[256];
static int aes_tablas_listas = 0;

static inline uint8_t aes_xtime(uint8_t x) {
  return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

static inline uint8_t aes_mul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) {
    if (b & 1) r ^= a;
    a = aes_xtime(a);
    b >>= 1;
  }
  return r;
}

static inline void aes_preparar_tablas(void) {
  if (aes_tablas_listas) return;
  for (int x = 0; x < 256; x++) {
    uint8_t inv = 0;
    if (x) {
      for (int y = 1; y < 256; y++) {
        if (aes_mul((uint8_t)x, (uint8_t)y) == 1) { inv = (uint8_t)y; break; }
      }
    }
    uint8_t s = inv, r = inv;
    for (int i = 0; i < 4; i++) {
      r = (uint8_t)((r << 1) | (r >> 7));
      s ^= r;
    }
    s ^= 0x63;
    aes_sbox[x] = s;
    aes_inv_sbox[s] = (uint8_t)x;
  }
  aes_tablas_listas = 1;
}

static inline void aes128_iniciar(aes128_ctx *c, const uint8_t clave[16]) {
  aes_preparar_tablas();
  memcpy(c->rk, clave, 16);
  uint8_t rcon = 1;
  for (int i = 16; i < 176; i += 4) {
    uint8_t t[4];
    memcpy(t, &c->rk[i - 4], 4);
    if (i % 16 == 0) {
      uint8_t a = t[0];
      t[0] = (uint8_t)(aes_sbox[t[1]] ^ rcon);
      t[1] = aes_sbox[t[2]];
      t[2] = aes_sbox[t[3]];
      t[3] = aes_sbox[a];
      rcon = aes_xtime(rcon);
    }
    for (int j = 0; j < 4; j++) c->rk[i + j] = (uint8_t)(c->rk[i - 16 + j] ^ t[j]);
  }
}

static inline void aes_add_rk(uint8_t s[16], const uint8_t *rk) {
  for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

// El estado se guarda en el orden de los bytes de entrada (columna por columna).
static inline void aes128_cifrar_bloque(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  memcpy(s, in, 16);
  aes_add_rk(s, c->rk);
  for (int ronda = 1; ronda <= 10; ronda++) {
    uint8_t t[16];
    for (int i = 0; i < 16; i++) t[i] = aes_sbox[s[i]];
    // ShiftRows
    for (int col = 0; col < 4; col++)
      for (int fila = 0; fila < 4; fila++)
        s[col * 4 + fila] = t[((col + fila) % 4) * 4 + fila];
    if (ronda != 10) {
      for (int col = 0; col < 4; col++) {
        uint8_t *p = &s[col * 4];
        uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (uint8_t)(aes_xtime(a0) ^ (aes_xtime(a1) ^ a1) ^ a2 ^ a3);
        p[1] = (uint8_t)(a0 ^ aes_xtime(a1) ^ (aes_xtime(a2) ^ a2) ^ a3);
        p[2] = (uint8_t)(a0 ^ a1 ^ aes_xtime(a2) ^ (aes_xtime(a3) ^ a3));
        p[3] = (uint8_t)((aes_xtime(a0) ^ a0) ^ a1 ^ a2 ^ aes_xtime(a3));
      }
    }
    aes_add_rk(s, c->rk + 16 * ronda);
  }
  memcpy(out, s, 16);
}

static inline void aes128_descifrar_bloque(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  memcpy(s, in, 16);
  aes_add_rk(s, c->rk + 160);
  for (int ronda = 9; ronda >= 0; ronda--) {
    uint8_t t[16];
    // InvShiftRows
    for (int col = 0; col < 4; col++)
      for (int fila = 0; fila < 4; fila++)
        t[((col + fila) % 4) * 4 + fila] = s[col * 4 + fila];
    for (int i = 0; i < 16; i++) s[i] = aes_inv_sbox[t[i]];
    aes_add_rk(s, c->rk + 16 * ronda);
    if (ronda != 0) {
      for (int col = 0; col < 4; col++) {
        uint8_t *p = &s[col * 4];
        uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (uint8_t)(aes_mul(a0, 14) ^ aes_mul(a1, 11) ^ aes_mul(a2, 13) ^ aes_mul(a3, 9));
        p[1] = (uint8_t)(aes_mul(a0, 9) ^ aes_mul(a1, 14) ^ aes_mul(a2, 11) ^ aes_mul(a3, 13));
        p[2] = (uint8_t)(aes_mul(a0, 13) ^ aes_mul(a1, 9) ^ aes_mul(a2, 14) ^ aes_mul(a3, 11));
        p[3] = (uint8_t)(aes_mul(a0, 11) ^ aes_mul(a1, 13) ^ aes_mul(a2, 9) ^ aes_mul(a3, 14));
      }
    }
  }
  memcpy(out, s, 16);
}

// CBC con IV en cero (como usa la NTAG 424 DNA en la autenticacion). n multiplo de 16.
// Se puede operar en el lugar (in == out).
static inline void aes128_cbc_cifrar(const aes128_ctx *c, const uint8_t iv[16],
                                     const uint8_t *in, uint8_t *out, size_t n) {
  uint8_t prev[16];
  memcpy(prev, iv, 16);
  for (size_t i = 0; i + 16 <= n; i += 16) {
    uint8_t b[16];
    for (int j = 0; j < 16; j++) b[j] = (uint8_t)(in[i + j] ^ prev[j]);
    aes128_cifrar_bloque(c, b, &out[i]);
    memcpy(prev, &out[i], 16);
  }
}

static inline void aes128_cbc_descifrar(const aes128_ctx *c, const uint8_t iv[16],
                                        const uint8_t *in, uint8_t *out, size_t n) {
  uint8_t prev[16];
  memcpy(prev, iv, 16);
  for (size_t i = 0; i + 16 <= n; i += 16) {
    uint8_t cifrado[16], b[16];
    memcpy(cifrado, &in[i], 16);
    aes128_descifrar_bloque(c, cifrado, b);
    for (int j = 0; j < 16; j++) out[i + j] = (uint8_t)(b[j] ^ prev[j]);
    memcpy(prev, cifrado, 16);
  }
}

// ---- AES-CMAC (RFC 4493) ----

static inline void cmac_desplazar(const uint8_t in[16], uint8_t out[16]) {
  uint8_t carry = 0;
  for (int i = 15; i >= 0; i--) {
    uint8_t nuevo = (uint8_t)((in[i] << 1) | carry);
    carry = (uint8_t)(in[i] >> 7);
    out[i] = nuevo;
  }
}

static inline void aes_cmac_subclaves(const aes128_ctx *c, uint8_t k1[16], uint8_t k2[16]) {
  uint8_t cero[16] = {0}, l[16];
  aes128_cifrar_bloque(c, cero, l);
  cmac_desplazar(l, k1);
  if (l[0] & 0x80) k1[15] ^= 0x87;
  cmac_desplazar(k1, k2);
  if (k1[0] & 0x80) k2[15] ^= 0x87;
}

static inline void aes_cmac(const uint8_t clave[16], const uint8_t *msg, size_t n, uint8_t mac[16]) {
  aes128_ctx c;
  aes128_iniciar(&c, clave);
  uint8_t k1[16], k2[16];
  aes_cmac_subclaves(&c, k1, k2);

  size_t bloques = (n + 15) / 16;
  int completo = (n != 0) && (n % 16 == 0);
  if (bloques == 0) bloques = 1;

  uint8_t ultimo[16];
  if (completo) {
    for (int i = 0; i < 16; i++) ultimo[i] = (uint8_t)(msg[(bloques - 1) * 16 + i] ^ k1[i]);
  } else {
    size_t resto = n - (bloques - 1) * 16;
    memset(ultimo, 0, 16);
    if (resto) memcpy(ultimo, &msg[(bloques - 1) * 16], resto);
    ultimo[resto] = 0x80;
    for (int i = 0; i < 16; i++) ultimo[i] ^= k2[i];
  }

  uint8_t x[16] = {0};
  for (size_t b = 0; b + 1 < bloques; b++) {
    for (int i = 0; i < 16; i++) x[i] ^= msg[b * 16 + i];
    aes128_cifrar_bloque(&c, x, x);
  }
  for (int i = 0; i < 16; i++) x[i] ^= ultimo[i];
  aes128_cifrar_bloque(&c, x, mac);
}

#ifdef __cplusplus
}
#endif
