#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#ifndef AURA_INGEST_ID_BYTES
#define AURA_INGEST_ID_BYTES 16
#endif

// El ingest_id de una muestra de la mesh es un UUID v4 que genera el NODO al
// tomar la muestra, y que se guarda con ella antes del primer envio (contrato
// §5). Asi es igual en cada reintento y despues de reiniciar cualquier placa,
// que es lo que permite al backend deduplicar en ts_telemetry.
//
// La v1 lo derivaba en el gateway de (MAC, boot_id, seq): con una cola
// persistente en el nodo eso ya no alcanza, porque la misma muestra puede
// reenviarse despues de reiniciar el gateway, con otro boot_id.

// Fuente de azar: esp_random() en la placa, un generador fijo en los tests.
typedef uint32_t (*AuraAzar)(void);

static inline void aura_ingest_id_nuevo(uint8_t id[AURA_INGEST_ID_BYTES], AuraAzar azar) {
  for (int i = 0; i < AURA_INGEST_ID_BYTES; i += 4) {
    uint32_t r = azar();
    id[i]     = (uint8_t)(r >> 24);
    id[i + 1] = (uint8_t)(r >> 16);
    id[i + 2] = (uint8_t)(r >> 8);
    id[i + 3] = (uint8_t)(r);
  }
  id[6] = (uint8_t)((id[6] & 0x0F) | 0x40);  // version 4
  id[8] = (uint8_t)((id[8] & 0x3F) | 0x80);  // variante RFC 9562
}

// salida debe tener al menos 37 bytes (36 + terminador).
static inline void aura_ingest_id_texto(const uint8_t b[AURA_INGEST_ID_BYTES], char salida[37]) {
  snprintf(salida, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
           b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
}

static inline bool aura_ingest_id_igual(const uint8_t a[AURA_INGEST_ID_BYTES],
                                        const uint8_t b[AURA_INGEST_ID_BYTES]) {
  return memcmp(a, b, AURA_INGEST_ID_BYTES) == 0;
}
