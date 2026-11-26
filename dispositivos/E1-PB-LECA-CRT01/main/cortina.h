/*
 * Cortina Luflex: motor NEMA 23 + TB67S109 y fin de carrera Hall.
 * Replica la logica del original en MicroPython (calibracion midiendo el recorrido contra el
 * Hall, 40 pasos de sobrerrecorrido, corte de seguridad al cerrar), pero sin bloquear al resto
 * del firmware: el movimiento corre en su propia tarea y un comando nuevo reemplaza al anterior.
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

/* Configura los GPIO y crea la tarea. calibrar_al_arrancar=false se usa en el firmware de banco.
   al_terminar se llama (desde la tarea del motor) al terminar un movimiento o una calibracion. */
void cortina_iniciar(bool calibrar_al_arrancar, cortina_cb_t al_terminar);

void cortina_calibrar(void);            /* pide una calibracion (asincrona) */
bool cortina_mover_pct(int pct);        /* 0..100. false si no esta calibrada o esta calibrando */
void cortina_parar(void);               /* aborta el movimiento o la calibracion en curso */

cortina_estado_t cortina_estado(void);
bool cortina_calibrada(void);
int  cortina_pos_pct(void);             /* -1 si no esta calibrada */
int  cortina_objetivo_pct(void);        /* ultimo destino pedido */
int  cortina_pasos_actuales(void);
int  cortina_pasos_totales(void);
const char *cortina_error(void);        /* NULL = sin error de calibracion */

/* Solo para pruebas de banco */
void cortina_banco_habilitar(bool en);
void cortina_banco_direccion(int nivel);
void cortina_banco_pasos(int n, uint32_t us);       /* bloqueante; n pasos en la direccion actual */
void cortina_banco_fijar_totales(int pasos);        /* fija la calibracion a mano */
int  cortina_hall(void);                            /* nivel del GPIO del Hall (0 = iman presente) */
void cortina_banco_pines(int step, int dir, int ena, int hall);   /* remapea los pines sin recompilar */
void cortina_banco_pines_actuales(int *step, int *dir, int *ena, int *hall);
