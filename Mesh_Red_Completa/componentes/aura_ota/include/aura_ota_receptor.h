/*
 * Receptor de actualizaciones por la malla: lo usan la hoja (dentro de nodo_mesh) y el
 * concentrador. Ver aura_ota.h para las tramas.
 *
 * Recibe la oferta del gateway y la acepta solo si es para este chip, de su mismo tipo (o con
 * "forzar") y de otra versión. Pide la imagen de a ventanas, la escribe en la partición OTA libre
 * mientras llega (sin borrarla entera antes), verifica el SHA-256 y la imagen, la deja para el
 * próximo arranque, avisa FIN y se reinicia. Lo de siempre sigue andando mientras tanto.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* Antes de reiniciarse con la versión nueva: dejar a salvo lo pendiente (cola, eventos).
       Puede tardar unos segundos. NULL = nada que guardar. */
    void (*antes_de_reiniciar)(void);
} aura_ota_receptor_cfg_t;

void aura_ota_receptor_iniciar(const aura_ota_receptor_cfg_t *cfg);
/* Desde la tarea de recepción, para cada trama que cumple aura_es_ota(): la copia y vuelve */
void aura_ota_recibir(const uint8_t *datos, size_t largo);
bool aura_ota_en_curso(void);
