#pragma once
#include <stdint.h>
#include <stdio.h>

// FNV-1a de 32 bits. Solo se usa para llenar los ultimos 4 bytes del UUID
// con algo que dependa de todo lo anterior; no es criptografico.
static inline uint32_t aura_fnv1a(const uint8_t* datos, int len) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; i++) {
    h ^= datos[i];
    h *= 16777619u;
  }
  return h;
}

// Arma un UUID determinístico a partir de (mac, boot_id, seq).
// La misma terna produce siempre la misma cadena: eso es lo que permite
// reintentar sin duplicar en el backend.
// salida debe tener al menos 37 bytes (36 + terminador).
static inline void aura_ingest_id(const uint8_t mac[6], uint32_t boot_id,
                                  uint16_t seq, char salida[37]) {
  uint8_t b[16];
  for (int i = 0; i < 6; i++) b[i] = mac[i];
  b[6]  = (uint8_t)(boot_id >> 24);
  b[7]  = (uint8_t)(boot_id >> 16);
  b[8]  = (uint8_t)(boot_id >> 8);
  b[9]  = (uint8_t)(boot_id);
  b[10] = (uint8_t)(seq >> 8);
  b[11] = (uint8_t)(seq);

  uint32_t h = aura_fnv1a(b, 12);
  b[12] = (uint8_t)(h >> 24);
  b[13] = (uint8_t)(h >> 16);
  b[14] = (uint8_t)(h >> 8);
  b[15] = (uint8_t)(h);

  snprintf(salida, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
           b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
}
