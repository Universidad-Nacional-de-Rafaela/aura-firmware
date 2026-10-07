#pragma once
#include "protocolo_aura.h"

// Cola de muestras del nodo en memoria no volatil (contrato §5): cada muestra
// se guarda ANTES del primer envio y sale de la cola solo cuando el raiz
// confirma que AURA la recibio (ack). Sobrevive a cortes del raiz, del WiFi, de
// AURA y del propio nodo, dentro de su capacidad.
//
// Formato: una cabecera y AURA_COLA_CAP ranuras de MuestraAura, en anillo. El
// almacenamiento esta abstraido: en la placa es un archivo de LittleFS y en
// los tests de host, un arreglo en RAM.
//
// Orden de escritura: primero la ranura, despues la cabecera. Si se corta la
// luz entre las dos, la cabecera vieja sigue siendo coherente y solo se pierde
// la muestra que se estaba guardando.

// 1440 muestras = 24 h a una por minuto, ~260 KB. La particion de datos del
// XIAO ESP32S3 es de 1,5 MB.
#ifndef AURA_COLA_CAP
#define AURA_COLA_CAP 1440
#endif

#define AURA_COLA_MAGIC   0x41555243u  // "AURC"
#define AURA_COLA_VERSION 1

typedef struct {
  bool (*leer)(void* ctx, uint32_t off, void* buf, uint32_t n);
  bool (*escribir)(void* ctx, uint32_t off, const void* buf, uint32_t n);
  void* ctx;
} AlmacenAura;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint16_t capacidad;
  uint32_t inicio;       // ranura de la muestra mas vieja
  uint32_t cantidad;
  uint32_t descartadas;  // perdidas por cola llena, desde que se formateo
  uint32_t control;      // FNV-1a de todo lo anterior
} CabeceraCola;

typedef struct {
  AlmacenAura  alm;
  CabeceraCola cab;
  bool         formateada;  // true si abrir() no encontro una cola valida
} ColaPersistente;

static inline uint32_t cola_bytes(uint32_t capacidad) {
  return (uint32_t)(sizeof(CabeceraCola) + capacidad * sizeof(MuestraAura));
}

static inline uint32_t cola_fnv1a(const uint8_t* datos, uint32_t len) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < len; i++) {
    h ^= datos[i];
    h *= 16777619u;
  }
  return h;
}

static inline uint32_t cola_control(const CabeceraCola* c) {
  return cola_fnv1a((const uint8_t*)c, (uint32_t)offsetof(CabeceraCola, control));
}

static inline uint32_t cola_offset_ranura(uint32_t ranura) {
  return (uint32_t)(sizeof(CabeceraCola) + ranura * sizeof(MuestraAura));
}

// Escribe la cabecera nueva; si falla, la cola en memoria queda como estaba.
static inline bool cola_guardar_cabecera(ColaPersistente* q, const CabeceraCola* nueva) {
  CabeceraCola c = *nueva;
  c.control = cola_control(&c);
  if (!q->alm.escribir(q->alm.ctx, 0, &c, sizeof(c))) return false;
  q->cab = c;
  return true;
}

// Abre la cola guardada. Si no hay una valida con esta capacidad (flash
// virgen, cabecera corrupta, otra capacidad), la formatea vacia y lo marca en
// formateada: no se interpreta basura como muestras.
static inline bool cola_abrir(ColaPersistente* q, AlmacenAura alm, uint32_t capacidad) {
  if (capacidad == 0 || capacidad > 0xFFFF) return false;
  q->alm = alm;
  q->formateada = false;

  CabeceraCola c;
  bool valida = alm.leer(alm.ctx, 0, &c, sizeof(c)) &&
                c.magic == AURA_COLA_MAGIC && c.version == AURA_COLA_VERSION &&
                c.capacidad == capacidad && c.inicio < capacidad &&
                c.cantidad <= capacidad && c.control == cola_control(&c);
  if (valida) {
    q->cab = c;
    return true;
  }

  CabeceraCola vacia;
  memset(&vacia, 0, sizeof(vacia));
  vacia.magic = AURA_COLA_MAGIC;
  vacia.version = AURA_COLA_VERSION;
  vacia.capacidad = (uint16_t)capacidad;
  q->formateada = true;
  return cola_guardar_cabecera(q, &vacia);
}

static inline uint32_t cola_cantidad(const ColaPersistente* q) { return q->cab.cantidad; }
static inline uint32_t cola_descartadas(const ColaPersistente* q) { return q->cab.descartadas; }

// Si esta llena, pisa la mas vieja: perder la muestra mas antigua es preferible
// a rechazar la mas reciente. Devuelve false solo si fallo la flash.
static inline bool cola_push(ColaPersistente* q, const MuestraAura* m) {
  CabeceraCola c = q->cab;
  if (c.cantidad == c.capacidad) {
    c.inicio = (c.inicio + 1) % c.capacidad;
    c.cantidad--;
    c.descartadas++;
  }
  uint32_t fin = (c.inicio + c.cantidad) % c.capacidad;
  if (!q->alm.escribir(q->alm.ctx, cola_offset_ranura(fin), m, sizeof(*m))) return false;
  c.cantidad++;
  return cola_guardar_cabecera(q, &c);
}

static inline bool cola_peek(const ColaPersistente* q, MuestraAura* salida) {
  if (q->cab.cantidad == 0) return false;
  return q->alm.leer(q->alm.ctx, cola_offset_ranura(q->cab.inicio), salida, sizeof(*salida));
}

// Saca la mas vieja. Se llama solo cuando el raiz confirmo esa muestra.
static inline bool cola_pop(ColaPersistente* q) {
  if (q->cab.cantidad == 0) return false;
  CabeceraCola c = q->cab;
  c.inicio = (c.inicio + 1) % c.capacidad;
  c.cantidad--;
  return cola_guardar_cabecera(q, &c);
}
