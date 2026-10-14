#pragma once
#include "protocolo_aura.h"

// 30 tramas x 185 bytes = 5550 bytes de RAM. El ESP32 tiene ~320 KB.
#define AURA_BUFFER_CAP 30

typedef struct {
  TramaAura items[AURA_BUFFER_CAP];
  uint16_t  inicio;       // indice del mas viejo
  uint16_t  cantidad;
  uint32_t  descartados;  // metrica: cuantos se perdieron por buffer lleno
} BufferCircular;

static inline void buffer_init(BufferCircular* b) {
  b->inicio = 0;
  b->cantidad = 0;
  b->descartados = 0;
}

static inline uint16_t buffer_cantidad(const BufferCircular* b) { return b->cantidad; }
static inline uint32_t buffer_descartados(const BufferCircular* b) { return b->descartados; }

// Si esta lleno, pisa el mas viejo. Nunca falla: perder la muestra mas
// antigua es preferible a rechazar la mas reciente.
static inline void buffer_push(BufferCircular* b, const TramaAura* t) {
  if (b->cantidad == AURA_BUFFER_CAP) {
    b->inicio = (uint16_t)((b->inicio + 1) % AURA_BUFFER_CAP);
    b->cantidad--;
    b->descartados++;
  }
  uint16_t fin = (uint16_t)((b->inicio + b->cantidad) % AURA_BUFFER_CAP);
  b->items[fin] = *t;
  b->cantidad++;
}

static inline bool buffer_peek(const BufferCircular* b, TramaAura* salida) {
  if (b->cantidad == 0) return false;
  *salida = b->items[b->inicio];
  return true;
}

static inline bool buffer_pop(BufferCircular* b, TramaAura* salida) {
  if (b->cantidad == 0) return false;
  *salida = b->items[b->inicio];
  b->inicio = (uint16_t)((b->inicio + 1) % AURA_BUFFER_CAP);
  b->cantidad--;
  return true;
}
