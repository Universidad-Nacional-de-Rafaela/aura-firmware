/*
 * Protocolo de líneas Pi -> hoja (sin dependencias de ESP-IDF, se prueba en la PC).
 *
 *   "P1 72" -> confiable, presencia, conf 72     "P0" -> confiable, sin presencia, conf -1
 *   "U"     -> no confiable                       otra cosa -> inválida
 */
#pragma once
#include <stdbool.h>

typedef struct {
    bool confiable;    /* false = la Pi avisó que la cámara no da una lectura confiable */
    bool presencia;
    int  conf_pct;     /* 0..100, o -1 si no vino */
} linea_pi_t;

/* true si la línea (sin \n) es válida; en ese caso completa *out. */
bool protocolo_pi_parsear(const char *linea, linea_pi_t *out);
