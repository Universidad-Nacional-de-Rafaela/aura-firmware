/*
 * Cola de mediciones de la hoja hasta que AURA las confirma: RAM primero, flash si hay un corte.
 *
 * - Cada medición entra a un buffer circular en RAM (COLA_CAPACIDAD). En marcha normal se
 *   confirma en menos de un segundo: la flash no se toca.
 * - Si una medición lleva COLA_A_FLASH_S sin confirmar (hay un corte de verdad), se copia a la
 *   partición "cola" (subtipo 0x40). Así sobrevive a un reinicio o a un corte de luz de la hoja.
 * - Al confirmarse, en la flash solo se cambia su byte de estado de 0xFF a 0x00: la flash NOR
 *   deja pasar bits de 1 a 0 sin borrar. Borrar (volver a 0xFF) se hace de a sectores de 4 KB y
 *   es lo que la desgasta: acá un sector se borra recién cuando el registro circular da toda
 *   la vuelta.
 * - Al arrancar, lo pendiente en la flash vuelve a la RAM, primero que todo.
 * - Llena la RAM, se pierde la más vieja ("descartadas").
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "aura_trama.h"

/* 400 mediciones ≈ 76 KB de RAM: ~66 min de corte midiendo cada 10 s */
#ifndef COLA_CAPACIDAD
#define COLA_CAPACIDAD 400
#endif
/* Sin confirmar tanto tiempo -> se copia a la flash */
#ifndef COLA_A_FLASH_S
#define COLA_A_FLASH_S 300
#endif

typedef struct {
    uint8_t  ingest_id[AURA_INGEST_ID_BYTES];
    uint32_t ts;          /* época UTC; 0 = la hoja no tenía hora */
    uint32_t uptime_s;    /* segundos desde el arranque: fecha la medición cuando llega la hora */
    uint8_t  otro_arranque; /* recuperada de la flash: su uptime_s ya no sirve */
    uint8_t  largo;       /* bytes de values */
    char     values[AURA_VALUES_MAX];
} cola_muestra_t;

esp_err_t cola_abrir(void);                    /* RAM + recupera lo pendiente de la flash */
bool cola_guardar(const cola_muestra_t *m);
bool cola_mirar(cola_muestra_t *m);            /* la más vieja, sin sacarla */
bool cola_sacar(void);                          /* saca la más vieja (confirmada) */
/* Cada pocos segundos: fecha las que se midieron sin hora (si ahora hay hora; epoca 0 = no hay)
   y copia a la flash las que pasaron COLA_A_FLASH_S sin confirmar. todas = copia todas las que
   están solo en RAM (antes de un reinicio a propósito, como el de una actualización). */
void cola_respaldar(uint32_t uptime_ahora, uint32_t epoca_ahora, bool todas);
uint32_t cola_cantidad(void);
uint32_t cola_descartadas(void);
uint32_t cola_en_flash(void);
uint32_t cola_capacidad(void);
