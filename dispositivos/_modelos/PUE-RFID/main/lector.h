#pragma once
/*
 * Lector RC522 (SPI) sobre el componente abobija/rc522 (version fija en idf_component.yml).
 * Cada vez que se apoya un llavero, deja su UID en la cola que se le pasa. El mismo codigo
 * lo usa el lector de banco (lector_banco/).
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lista_tarjetas.h"

typedef struct {
    uint8_t largo;
    uint8_t uid[UID_MAX_BYTES];
} lectura_t;

typedef enum {
    LECTOR_OK = 0,
    LECTOR_RECUPERADO,      /* estaba apagado (se reinicio) y se lo volvio a levantar */
    LECTOR_CAIDO            /* no responde ni reiniciandolo */
} lector_salud_t;

/* Arranca el SPI y el lector. Cada lectura va a `cola` (de elementos lectura_t). */
esp_err_t lector_iniciar(QueueHandle_t cola);

/* Revisa que el RC522 siga vivo (version y antena) y, si se reinicio, lo levanta. Llamar cada
   unos segundos desde una tarea que no sea la del lector. */
lector_salud_t lector_revisar(void);
