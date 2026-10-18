#pragma once
#include "cola_persistente.h"

// La cola persistente repartida en archivos chicos.
//
// LittleFS es copy-on-write: cambiar unos bytes en medio de un archivo
// reescribe el archivo desde ahi hasta el final. Con la cola en un solo archivo
// de ~260 KB, cada escritura de la cabecera (offset 0) reescribia todo: ~2,5 s
// con el loop parado y ~180 bloques borrados por muestra (medido en placa el
// 2026-10-17). Repartida, cada escritura toca un solo archivo de menos de un
// bloque.
//
// El archivo 0 es la cabecera; del 1 en adelante, AURA_COLA_RANURAS_POR_ARCHIVO
// ranuras cada uno. cola_persistente.h no se entera: sigue viendo un espacio de
// offsets continuo, y el orden "ranura, despues cabecera" se mantiene porque
// cada escritura es un archivo distinto que se cierra antes de seguir.

// 16 x 181 bytes = 2,9 KB: entra en un bloque de 4 KB de LittleFS.
#ifndef AURA_COLA_RANURAS_POR_ARCHIVO
#define AURA_COLA_RANURAS_POR_ARCHIVO 16
#endif

#define AURA_COLA_BYTES_ARCHIVO \
  ((uint32_t)(AURA_COLA_RANURAS_POR_ARCHIVO * sizeof(MuestraAura)))

typedef struct {
  bool (*leer)(void* ctx, uint32_t archivo, uint32_t off, void* buf, uint32_t n);
  bool (*escribir)(void* ctx, uint32_t archivo, uint32_t off, const void* buf, uint32_t n);
  void* ctx;
} ArchivosAura;

// Traduce el offset de la cola a archivo y offset dentro de el. Devuelve
// cuantos de los n bytes caen en ese archivo: el resto va al siguiente.
static inline uint32_t cola_tramo(uint32_t off, uint32_t n, uint32_t* archivo, uint32_t* off_archivo) {
  const uint32_t cabecera = (uint32_t)sizeof(CabeceraCola);
  uint32_t lugar;
  if (off < cabecera) {
    *archivo = 0;
    *off_archivo = off;
    lugar = cabecera - off;
  } else {
    uint32_t datos = off - cabecera;
    *archivo = 1 + datos / AURA_COLA_BYTES_ARCHIVO;
    *off_archivo = datos % AURA_COLA_BYTES_ARCHIVO;
    lugar = AURA_COLA_BYTES_ARCHIVO - *off_archivo;
  }
  return n < lugar ? n : lugar;
}

static inline uint32_t cola_archivos_cantidad(uint32_t capacidad) {
  return 1 + (capacidad + AURA_COLA_RANURAS_POR_ARCHIVO - 1) / AURA_COLA_RANURAS_POR_ARCHIVO;
}

// Tamano final de cada archivo, para crearlos de una vez al arrancar.
static inline uint32_t cola_archivo_bytes(uint32_t archivo, uint32_t capacidad) {
  if (archivo == 0) return (uint32_t)sizeof(CabeceraCola);
  uint32_t primera = (archivo - 1) * AURA_COLA_RANURAS_POR_ARCHIVO;
  if (primera >= capacidad) return 0;
  uint32_t ranuras = capacidad - primera;
  if (ranuras > AURA_COLA_RANURAS_POR_ARCHIVO) ranuras = AURA_COLA_RANURAS_POR_ARCHIVO;
  return ranuras * (uint32_t)sizeof(MuestraAura);
}

static inline bool cola_archivos_leer(const ArchivosAura* a, uint32_t off, void* buf, uint32_t n) {
  uint8_t* p = (uint8_t*)buf;
  while (n > 0) {
    uint32_t archivo, off_archivo;
    uint32_t parte = cola_tramo(off, n, &archivo, &off_archivo);
    if (!a->leer(a->ctx, archivo, off_archivo, p, parte)) return false;
    off += parte; p += parte; n -= parte;
  }
  return true;
}

static inline bool cola_archivos_escribir(const ArchivosAura* a, uint32_t off, const void* buf, uint32_t n) {
  const uint8_t* p = (const uint8_t*)buf;
  while (n > 0) {
    uint32_t archivo, off_archivo;
    uint32_t parte = cola_tramo(off, n, &archivo, &off_archivo);
    if (!a->escribir(a->ctx, archivo, off_archivo, p, parte)) return false;
    off += parte; p += parte; n -= parte;
  }
  return true;
}

static inline bool cola_archivos_leer_alm(void* ctx, uint32_t off, void* buf, uint32_t n) {
  return cola_archivos_leer((const ArchivosAura*)ctx, off, buf, n);
}

static inline bool cola_archivos_escribir_alm(void* ctx, uint32_t off, const void* buf, uint32_t n) {
  return cola_archivos_escribir((const ArchivosAura*)ctx, off, buf, n);
}

// El AlmacenAura que usa la cola. archivos tiene que vivir mientras viva ella.
static inline AlmacenAura cola_archivos_almacen(ArchivosAura* archivos) {
  AlmacenAura a = {cola_archivos_leer_alm, cola_archivos_escribir_alm, archivos};
  return a;
}
