#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

// Partes de nodo_mesh.h que no dependen de la placa, para probarlas en host.

// ===== Alertas de sonda, una por cambio (contrato §3.5) =====
// Una sonda que falla durante una hora es UNA alerta, no sesenta. El estado
// vive en RAM: despues de reiniciar, una sonda que sigue fallando vuelve a
// avisar una vez, y eso tambien es informacion.
#define AURA_SONDAS_MAX  8
#define AURA_CAMPO_MAX   24   // nombre de campo de values, con terminador

typedef struct {
  char campo[AURA_CAMPO_MAX];
  bool falla;
} EstadoSonda;

typedef struct {
  EstadoSonda s[AURA_SONDAS_MAX];
  uint8_t     n;
} EstadoSondas;

typedef enum {
  AURA_SONDA_SIN_CAMBIO = 0,
  AURA_SONDA_FALLO,        // publicar alerta "sensor" (sin_respuesta / fuera_de_rango)
  AURA_SONDA_RECUPERADA,   // publicar alerta "sensor" con severity info, motivo recuperada
  AURA_SONDA_SIN_LUGAR     // no se puede seguir esta sonda: tabla llena o nombre largo
} AuraCambioSonda;

static inline void aura_sondas_init(EstadoSondas* e) { memset(e, 0, sizeof(*e)); }

static inline AuraCambioSonda aura_sonda_cambio(EstadoSondas* e, const char* campo, bool ok) {
  for (uint8_t i = 0; i < e->n; i++) {
    if (strcmp(e->s[i].campo, campo) != 0) continue;
    if (e->s[i].falla == !ok) return AURA_SONDA_SIN_CAMBIO;
    e->s[i].falla = !ok;
    return ok ? AURA_SONDA_RECUPERADA : AURA_SONDA_FALLO;
  }
  if (e->n == AURA_SONDAS_MAX || strlen(campo) >= AURA_CAMPO_MAX) return AURA_SONDA_SIN_LUGAR;
  EstadoSonda* s = &e->s[e->n++];
  strcpy(s->campo, campo);
  s->falla = !ok;
  return ok ? AURA_SONDA_SIN_CAMBIO : AURA_SONDA_FALLO;
}

// ===== Reintento de una muestra sin confirmar =====
// 15 s, 30 s, 1 min... hasta 5 min. Con AURA caida horas, cada nodo pregunta
// a lo sumo cada 5 minutos en vez de saturar el gateway.
#define AURA_ESPERA_MIN_MS 15000u
#define AURA_ESPERA_MAX_MS 300000u

static inline uint32_t aura_espera_confirmacion_ms(uint32_t intentos) {
  if (intentos >= 5) return AURA_ESPERA_MAX_MS;
  uint32_t ms = AURA_ESPERA_MIN_MS << intentos;
  return ms > AURA_ESPERA_MAX_MS ? AURA_ESPERA_MAX_MS : ms;
}
