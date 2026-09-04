#pragma once
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define AURA_PROTO_VERSION 1
#define AURA_PAYLOAD_MAX   180
#define AURA_CABECERA_BYTES 17

// Tipos de mensaje. Se transmiten como uint8_t, no cambiar los valores.
enum {
  AURA_TIPO_TELEMETRIA = 1,
  AURA_TIPO_COMANDO    = 2,
  AURA_TIPO_ACK        = 3
};

// __attribute__((packed)) evita relleno entre campos: emisor y receptor
// tienen que interpretar exactamente los mismos bytes.
typedef struct __attribute__((packed)) {
  uint8_t  version;
  uint8_t  tipo;
  uint8_t  mac_origen[6];    // quien genero el mensaje
  uint8_t  mac_destino[6];   // destinatario FINAL, no el proximo salto
  uint16_t seq;              // secuencia por nodo origen, para ACK e ingest_id
  uint8_t  largo;            // bytes utiles en payload
  uint8_t  payload[AURA_PAYLOAD_MAX];
} TramaAura;

static inline void aura_trama_init(TramaAura* t, uint8_t tipo,
                                   const uint8_t origen[6],
                                   const uint8_t destino[6],
                                   uint16_t seq,
                                   const uint8_t* payload, uint8_t largo) {
  memset(t, 0, sizeof(*t));
  t->version = AURA_PROTO_VERSION;
  t->tipo    = tipo;
  memcpy(t->mac_origen,  origen,  6);
  memcpy(t->mac_destino, destino, 6);
  t->seq   = seq;
  t->largo = (largo > AURA_PAYLOAD_MAX) ? AURA_PAYLOAD_MAX : largo;
  if (payload && t->largo) memcpy(t->payload, payload, t->largo);
}

// Cuantos bytes hay que mandar realmente por la radio.
static inline uint16_t aura_trama_bytes(const TramaAura* t) {
  return (uint16_t)(AURA_CABECERA_BYTES + t->largo);
}

// Valida lo que llega del aire ANTES de interpretarlo.
static inline bool aura_trama_valida(const uint8_t* datos, int len) {
  if (datos == NULL) return false;
  if (len < AURA_CABECERA_BYTES) return false;
  const TramaAura* t = (const TramaAura*)datos;
  if (t->version != AURA_PROTO_VERSION) return false;
  if (t->largo > AURA_PAYLOAD_MAX) return false;
  if (len != AURA_CABECERA_BYTES + t->largo) return false;
  return true;
}

static inline bool aura_es_para_mi(const TramaAura* t, const uint8_t mi_mac[6]) {
  return memcmp(t->mac_destino, mi_mac, 6) == 0;
}
