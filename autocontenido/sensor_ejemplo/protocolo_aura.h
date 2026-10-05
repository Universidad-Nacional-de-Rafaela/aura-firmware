#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

// Version de la trama ESP-NOW entre nodos. No es la version del contrato MQTT:
// la trama es interna de la mesh y AURA no la ve (contrato §7).
// v2 (contrato v3.0): ingest_id generado en el nodo y confirmacion de punta a punta.
#define AURA_PROTO_VERSION 2
#define AURA_PAYLOAD_MAX   180
#define AURA_CABECERA_BYTES 17

// Tipos de mensaje. Se transmiten como uint8_t: no cambiar los valores, solo agregar.
enum {
  AURA_TIPO_TELEMETRIA   = 1,  // nodo -> gateway: una muestra de la cola del nodo
  AURA_TIPO_COMANDO      = 2,  // gateway -> nodo: el JSON del backend tal cual
  AURA_TIPO_ACK          = 3,  // salto a salto: "recibi tu trama", nada mas
  AURA_TIPO_CONFIRMACION = 4,  // gateway -> nodo: AURA persistio esa muestra
  AURA_TIPO_RESULTADO    = 5,  // nodo -> gateway: que paso con un comando
  AURA_TIPO_REPORTE      = 6,  // nodo -> gateway: config vigente y estado de la cola
  AURA_TIPO_ALERTA       = 7,  // nodo -> gateway: alerts/<id>/<tipo>
  AURA_TIPO_PING         = 8   // al padre, durante el barrido de canales
};

// __attribute__((packed)) evita relleno entre campos: emisor y receptor
// tienen que interpretar exactamente los mismos bytes.
typedef struct __attribute__((packed)) {
  uint8_t  version;
  uint8_t  tipo;
  uint8_t  mac_origen[6];    // quien genero el mensaje
  uint8_t  mac_destino[6];   // destinatario FINAL, no el proximo salto
  uint16_t seq;              // secuencia por nodo origen, para el ACK de salto
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

// Resultado de validar lo que llega del aire. VERSION va aparte porque no es
// ruido: es un nodo con firmware de otra generacion, y hay que contarlo para
// que no se pierda en silencio.
typedef enum {
  AURA_TRAMA_OK = 0,
  AURA_TRAMA_CORTA,
  AURA_TRAMA_VERSION,
  AURA_TRAMA_LARGO
} AuraValidacion;

// Valida lo que llega del aire ANTES de interpretarlo.
static inline AuraValidacion aura_trama_validar(const uint8_t* datos, int len) {
  if (datos == NULL || len < AURA_CABECERA_BYTES) return AURA_TRAMA_CORTA;
  const TramaAura* t = (const TramaAura*)datos;
  if (t->version != AURA_PROTO_VERSION) return AURA_TRAMA_VERSION;
  if (t->largo > AURA_PAYLOAD_MAX) return AURA_TRAMA_LARGO;
  if (len != AURA_CABECERA_BYTES + t->largo) return AURA_TRAMA_LARGO;
  return AURA_TRAMA_OK;
}

static inline bool aura_trama_valida(const uint8_t* datos, int len) {
  return aura_trama_validar(datos, len) == AURA_TRAMA_OK;
}

static inline bool aura_es_para_mi(const TramaAura* t, const uint8_t mi_mac[6]) {
  return memcmp(t->mac_destino, mi_mac, 6) == 0;
}

// Sentido de cada tipo. Un relevo (la sala) reenvia lo que sube al gateway y
// lo que baja al hijo que figura en mac_destino. ACK y PING son de un salto:
// no suben ni bajan.
static inline bool aura_tipo_sube(uint8_t tipo) {
  return tipo == AURA_TIPO_TELEMETRIA || tipo == AURA_TIPO_RESULTADO ||
         tipo == AURA_TIPO_REPORTE    || tipo == AURA_TIPO_ALERTA;
}

static inline bool aura_tipo_baja(uint8_t tipo) {
  return tipo == AURA_TIPO_COMANDO || tipo == AURA_TIPO_CONFIRMACION;
}

// Copia el payload a un buffer terminado en cero. El payload viene del aire y
// no trae terminador.
static inline void aura_payload_texto(const TramaAura* t, char* salida, size_t cap) {
  if (cap == 0) return;
  size_t n = t->largo;
  if (n >= cap) n = cap - 1;
  memcpy(salida, t->payload, n);
  salida[n] = '\0';
}

// ===== Hora =====
// 2026-01-01T00:00:00Z. Una hora anterior es un RTC que nunca se ajusto.
#define AURA_EPOCA_MINIMA 1767225600u

static inline bool aura_hora_valida(uint32_t epoca) { return epoca >= AURA_EPOCA_MINIMA; }

// uint32 en little-endian explicito: no depende de la alineacion del payload.
static inline void aura_u32_escribir(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t aura_u32_leer(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ===== Telemetria =====
// Payload: ingest_id (16 B) | ts (uint32 LE, 0 = sin hora) | JSON de values.
// El ingest_id lo genera el nodo y se guarda con la muestra antes del primer
// envio (contrato §5): es igual en cada reintento y despues de reiniciar.
#define AURA_INGEST_ID_BYTES     16
#define AURA_TELEMETRIA_CABECERA (AURA_INGEST_ID_BYTES + 4)
#define AURA_VALUES_MAX          (AURA_PAYLOAD_MAX - AURA_TELEMETRIA_CABECERA)

// Una muestra tal como la guarda la cola del nodo.
typedef struct __attribute__((packed)) {
  uint8_t  ingest_id[AURA_INGEST_ID_BYTES];
  uint32_t ts;
  uint8_t  largo;                    // bytes utiles de values, sin terminador
  char     values[AURA_VALUES_MAX];  // JSON del objeto values, sin terminador
} MuestraAura;

static inline bool aura_telemetria_armar(TramaAura* t, const uint8_t origen[6],
                                         const uint8_t destino[6], uint16_t seq,
                                         const MuestraAura* m) {
  if (m->largo > AURA_VALUES_MAX) return false;
  uint8_t p[AURA_PAYLOAD_MAX];
  memcpy(p, m->ingest_id, AURA_INGEST_ID_BYTES);
  aura_u32_escribir(p + AURA_INGEST_ID_BYTES, m->ts);
  memcpy(p + AURA_TELEMETRIA_CABECERA, m->values, m->largo);
  aura_trama_init(t, AURA_TIPO_TELEMETRIA, origen, destino, seq, p,
                  (uint8_t)(AURA_TELEMETRIA_CABECERA + m->largo));
  return true;
}

static inline bool aura_telemetria_leer(const TramaAura* t, MuestraAura* m) {
  if (t->tipo != AURA_TIPO_TELEMETRIA || t->largo < AURA_TELEMETRIA_CABECERA) return false;
  memset(m, 0, sizeof(*m));
  memcpy(m->ingest_id, t->payload, AURA_INGEST_ID_BYTES);
  m->ts = aura_u32_leer(t->payload + AURA_INGEST_ID_BYTES);
  m->largo = (uint8_t)(t->largo - AURA_TELEMETRIA_CABECERA);
  memcpy(m->values, t->payload + AURA_TELEMETRIA_CABECERA, m->largo);
  return true;
}

// ===== Confirmacion =====
// Payload: ingest_id (16 B) | hora del gateway (uint32 LE, 0 = sin hora).
// La manda el gateway SOLO cuando AURA dijo que la muestra quedo persistida
// (contrato §6). Lleva la hora porque los nodos no tienen otra forma de saberla.
#define AURA_CONFIRMACION_BYTES (AURA_INGEST_ID_BYTES + 4)

static inline void aura_confirmacion_armar(TramaAura* t, const uint8_t origen[6],
                                           const uint8_t destino[6], uint16_t seq,
                                           const uint8_t ingest_id[AURA_INGEST_ID_BYTES],
                                           uint32_t hora) {
  uint8_t p[AURA_CONFIRMACION_BYTES];
  memcpy(p, ingest_id, AURA_INGEST_ID_BYTES);
  aura_u32_escribir(p + AURA_INGEST_ID_BYTES, hora);
  aura_trama_init(t, AURA_TIPO_CONFIRMACION, origen, destino, seq, p, AURA_CONFIRMACION_BYTES);
}

static inline bool aura_confirmacion_leer(const TramaAura* t,
                                          uint8_t ingest_id[AURA_INGEST_ID_BYTES],
                                          uint32_t* hora) {
  if (t->tipo != AURA_TIPO_CONFIRMACION || t->largo != AURA_CONFIRMACION_BYTES) return false;
  memcpy(ingest_id, t->payload, AURA_INGEST_ID_BYTES);
  *hora = aura_u32_leer(t->payload + AURA_INGEST_ID_BYTES);
  return true;
}
