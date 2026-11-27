/* Logica pura del modo automatico (sin hardware, para poder probarla en la PC). */
#pragma once

#define MODO_MANUAL 0
#define MODO_AUTO1  1   /* oscuro -> cerrada, mucha luz -> abierta */
#define MODO_AUTO2  2   /* inverso: mucha luz -> cerrada (protege del sol), oscuro -> abierta */

/* Posicion sugerida (0, 50 o 100 %) para el modo y la luz dados, igual que el original:
   tres niveles separados por dos umbrales. */
static inline int logica_sugerencia(int modo, double lux, double umbral_bajo, double umbral_alto)
{
    if (modo == MODO_AUTO1) {
        if (lux < umbral_bajo) return 0;
        if (lux < umbral_alto) return 50;
        return 100;
    }
    /* MODO_AUTO2 */
    if (lux > umbral_alto) return 0;
    if (lux >= umbral_bajo) return 50;
    return 100;
}

/* Antirrebote: la sugerencia tiene que repetirse "necesarias" lecturas seguidas antes de actuar.
   estado->ultima y estado->cuenta guardan la historia. Devuelve true cuando hay que mover. */
typedef struct { int ultima; int cuenta; } logica_confirmacion_t;

static inline int logica_confirmar(logica_confirmacion_t *c, int sugerida, int objetivo_actual, int necesarias)
{
    if (sugerida == objetivo_actual) { c->cuenta = 0; return 0; }
    if (sugerida == c->ultima) c->cuenta++;
    else { c->ultima = sugerida; c->cuenta = 0; }
    if (c->cuenta >= necesarias) { c->cuenta = 0; return 1; }
    return 0;
}
