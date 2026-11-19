#pragma once
/*
 * Modo autonomo de la estacion de enrolamiento, sin hardware (se prueba en la compu): un boton y
 * un buzzer deciden y avisan, sin PC.
 *
 *   boton suelto                      -> apoyar una tarjeta la ENROLA
 *   boton apretado (desde antes)      -> apoyar una tarjeta la BORRA (vuelve a clave de fabrica)
 *
 * Borrar es lo que puede dejar una tarjeta inservible si sale mal, asi que pide intencion: el boton
 * tiene que estar apretado EST_BOTON_MIN_MS antes de que aparezca la tarjeta. Un roce al pasar no
 * alcanza, y el borrado nunca es el modo por defecto.
 */
#include <stdbool.h>
#include <stdint.h>
#include "enrolador_logica.h"

#define EST_BOTON_MIN_MS 400
#define EST_AUSENCIAS_PARA_RETIRO 3     /* lecturas seguidas sin tarjeta para darla por retirada */
#define EST_PERIODO_RETIRO_MS 150       /* cada cuanto se mira si sigue ahi: ~0,5 s de ausencia real */
#define EST_PAUSA_TRAS_RETIRO_MS 500    /* y despues de retirada, un respiro antes de atender otra */

typedef enum { EST_ENROLAR = 0, EST_BORRAR } est_modo_t;

typedef struct {
  bool apretado;
  uint32_t desde;       /* ms en que se apreto */
} est_boton_t;

/* `nivel_apretado`: el pin lee "apretado" (con la pull-up interna, el pin en BAJO). */
static inline void est_boton_actualizar(est_boton_t *b, bool nivel_apretado, uint32_t ahora_ms) {
  if (nivel_apretado && !b->apretado) b->desde = ahora_ms;
  b->apretado = nivel_apretado;
}

/* Cuanto hace que esta apretado (0 si esta suelto). */
static inline uint32_t est_boton_ms(const est_boton_t *b, uint32_t ahora_ms) {
  return b->apretado ? (uint32_t)(ahora_ms - b->desde) : 0;
}

static inline est_modo_t est_modo(const est_boton_t *b, uint32_t ahora_ms) {
  return est_boton_ms(b, ahora_ms) >= EST_BOTON_MIN_MS ? EST_BORRAR : EST_ENROLAR;
}

typedef enum {
  EST_SON_ENROLADA = 0,   /* dos tonos que suben */
  EST_SON_BORRADA,        /* dos tonos que bajan */
  EST_SON_YA,             /* un tono corto: ya estaba asi, no se toco nada */
  EST_SON_ERROR,          /* un tono grave largo: no se pudo, la tarjeta no se toco o sigue como estaba */
  EST_SON_PELIGRO         /* grave y agudo alternados: la tarjeta puede haber quedado mal, mirarla */
} est_sonido_t;

static inline est_sonido_t est_sonido(est_modo_t modo, enr_resultado_t r) {
  if (r == ENR_INCIERTA || r == ENR_POSIBLEMENTE_PERDIDA) return EST_SON_PELIGRO;
  if (modo == EST_ENROLAR) {
    if (r == ENR_ENROLADA_OK) return EST_SON_ENROLADA;
    if (r == ENR_YA_ENROLADA) return EST_SON_YA;
  } else {
    if (r == ENR_DESENROLADA_OK) return EST_SON_BORRADA;
    if (r == ENR_YA_DE_FABRICA) return EST_SON_YA;
  }
  return EST_SON_ERROR;
}

/* Cuenta lecturas sin tarjeta; true cuando ya se retiro. Con tarjeta presente reinicia la cuenta. */
static inline bool est_retirada(int *ausencias, bool presente) {
  if (presente) { *ausencias = 0; return false; }
  return ++(*ausencias) >= EST_AUSENCIAS_PARA_RETIRO;
}
