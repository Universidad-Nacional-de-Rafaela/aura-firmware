/*
 * Cortina Luflex: motor NEMA 23 + TB67S109 y fin de carrera mecanico (microswitch en 0 %).
 * El recorrido total (pasos_totales) es configurable (no se infiere desde la posicion de arranque).
 * Al arrancar o al pedir homing, busca el microswitch de 0 % sin empujar contra el tope.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CORTINA_DETENIDA = 0,
    CORTINA_MOVIENDO = 1,
    CORTINA_CALIBRANDO = 2,
} cortina_estado_t;

typedef void (*cortina_cb_t)(void);

/* Configura los GPIO y crea la tarea. pasos_totales_inicial define el recorrido (0 -> 100 %).
   homing_al_arrancar=false se usa en el firmware de banco. */
void cortina_iniciar(int pasos_totales_inicial, bool homing_al_arrancar, cortina_cb_t al_terminar);

void cortina_fijar_pasos_totales(int pasos); /* actualiza el recorrido (desde set_config o banco) */
void cortina_homing(void);                   /* busca el microswitch de 0 % de forma asincrona */
bool cortina_mover_pct(int pct);             /* 0..100. false si todavia no hizo homing */
void cortina_parar(void);                    /* aborta el movimiento o homing en curso */

cortina_estado_t cortina_estado(void);
bool cortina_calibrada(void);
int  cortina_pos_pct(void);                  /* -1 si no hizo homing */
int  cortina_objetivo_pct(void);
int  cortina_pasos_actuales(void);
int  cortina_pasos_totales(void);
const char *cortina_error(void);             /* NULL = sin error */

/* Solo para pruebas de banco */
void cortina_banco_habilitar(bool en);
void cortina_banco_direccion(int nivel);
void cortina_banco_pasos(int n, uint32_t us);
int  cortina_tope(void);                     /* nivel del microswitch (0 = activado, 1 = libre) */
void cortina_banco_pines(int step, int dir, int ena, int tope);
void cortina_banco_pines_actuales(int *step, int *dir, int *ena, int *tope);
