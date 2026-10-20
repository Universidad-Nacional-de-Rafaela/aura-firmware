/*
 * Cola de mediciones de la hoja hasta que AURA las confirma, en RAM.
 *
 * Es un buffer circular de COLA_CAPACIDAD mediciones (~190 B cada una). En marcha normal
 * cada medición se confirma en menos de un segundo y la cola tiene 0 o 1. En un corte (WiFi,
 * broker, AURA, concentrador) acumula; llena, se pierde la más vieja y se cuenta en
 * "descartadas". No escribe la flash: lo que no se confirmó se pierde si la hoja se reinicia
 * o se queda sin alimentación (decisión de diseño del 2026-10-20).
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

typedef struct {
    uint8_t  ingest_id[AURA_INGEST_ID_BYTES];
    uint32_t ts;          /* época UTC; 0 = la hoja no tenía hora */
    uint32_t uptime_s;    /* segundos desde el arranque: fecha la medición cuando llega la hora */
    uint8_t  largo;       /* bytes de values */
    char     values[AURA_VALUES_MAX];
} cola_muestra_t;

esp_err_t cola_abrir(void);
bool cola_guardar(const cola_muestra_t *m);
bool cola_mirar(cola_muestra_t *m);           /* la más vieja, sin sacarla */
bool cola_sacar(void);                         /* saca la más vieja */
uint32_t cola_cantidad(void);
uint32_t cola_descartadas(void);
uint32_t cola_capacidad(void);
