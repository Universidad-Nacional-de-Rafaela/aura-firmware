#pragma once
/*
 * SHA-256 y HMAC-SHA256 en C puro, para probar en host la derivacion de la clave por tarjeta.
 * En la placa NO se usa esto: la clave maestra vive en un eFuse y el HMAC lo calcula el
 * periferico (esp_hmac_calculate) sin que la clave salga del chip. Este header sirve para
 * (a) tests de host y (b) la estacion de enrolamiento mientras no tenga eFuse.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t h[8];
  uint8_t buf[64];
  size_t nbuf;
  uint64_t total;
} sha256_ctx;

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t sha_rotd(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static inline void sha256_bloque(sha256_ctx *c, const uint8_t b[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)b[4 * i] << 24) | ((uint32_t)b[4 * i + 1] << 16) | ((uint32_t)b[4 * i + 2] << 8) | b[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = sha_rotd(w[i - 15], 7) ^ sha_rotd(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = sha_rotd(w[i - 2], 17) ^ sha_rotd(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->h[0], bb = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = sha_rotd(e, 6) ^ sha_rotd(e, 11) ^ sha_rotd(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
    uint32_t S0 = sha_rotd(a, 2) ^ sha_rotd(a, 13) ^ sha_rotd(a, 22);
    uint32_t mj = (a & bb) ^ (a & cc) ^ (bb & cc);
    uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1; d = cc; cc = bb; bb = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += bb; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static inline void sha256_iniciar(sha256_ctx *c) {
  static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(c->h, h0, sizeof(h0));
  c->nbuf = 0;
  c->total = 0;
}

static inline void sha256_agregar(sha256_ctx *c, const uint8_t *d, size_t n) {
  c->total += n;
  while (n) {
    size_t t = 64 - c->nbuf;
    if (t > n) t = n;
    memcpy(&c->buf[c->nbuf], d, t);
    c->nbuf += t; d += t; n -= t;
    if (c->nbuf == 64) { sha256_bloque(c, c->buf); c->nbuf = 0; }
  }
}

static inline void sha256_fin(sha256_ctx *c, uint8_t out[32]) {
  uint64_t bits = c->total * 8;
  uint8_t p = 0x80;
  sha256_agregar(c, &p, 1);
  p = 0;
  while (c->nbuf != 56) sha256_agregar(c, &p, 1);
  uint8_t l[8];
  for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha256_agregar(c, l, 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(c->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)c->h[i];
  }
}

static inline void hmac_sha256(const uint8_t *clave, size_t nc, const uint8_t *msg, size_t n, uint8_t out[32]) {
  uint8_t k[64] = {0};
  if (nc > 64) {
    sha256_ctx c;
    sha256_iniciar(&c);
    sha256_agregar(&c, clave, nc);
    sha256_fin(&c, k);
  } else {
    memcpy(k, clave, nc);
  }
  uint8_t ip[64], op[64], interno[32];
  for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
  sha256_ctx c;
  sha256_iniciar(&c);
  sha256_agregar(&c, ip, 64);
  sha256_agregar(&c, msg, n);
  sha256_fin(&c, interno);
  sha256_iniciar(&c);
  sha256_agregar(&c, op, 64);
  sha256_agregar(&c, interno, 32);
  sha256_fin(&c, out);
}

// Clave AES-128 de una tarjeta: primeros 16 bytes de HMAC-SHA256(clave_maestra, UID).
// uid_len en {4, 7, 10}. DECISION PENDIENTE con el profe: el documento dice solo
// "HMAC-SHA256(clave_eFuse, UID)"; truncar a 16 bytes y no agregar etiqueta de dominio
// es lo que asume este codigo.
static inline int clave_de_tarjeta(const uint8_t maestra[32], const uint8_t *uid, size_t uid_len, uint8_t out[16]) {
  if (uid_len != 4 && uid_len != 7 && uid_len != 10) return -1;
  uint8_t h[32];
  hmac_sha256(maestra, 32, uid, uid_len, h);
  memcpy(out, h, 16);
  return 0;
}

#ifdef __cplusplus
}
#endif
