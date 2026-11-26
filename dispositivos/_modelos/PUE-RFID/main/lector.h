#pragma once
/*
 * Lector RC522 (SPI) con driver propio (pcd_rc522.c) y la logica de tarjeta de comun/. Cada vez
 * que se apoya un llavero, deja su UID en la cola que se le pasa. Si hay un verificador
 * (lector_set_verificador), con la tarjeta todavia en el campo lo llama para que la autentique
 * (NTAG 424 DNA) y el resultado viaja en la lectura. El mismo codigo lo usa lector_banco/.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lector_logica.h"
#include "lista_tarjetas.h"

#define LECTOR_AUTH_SIN_VERIFICADOR (-1)

typedef struct {
    uint8_t largo;
    uint8_t uid[UID_MAX_BYTES];
    int8_t auth;        /* lo que devolvio el verificador, o LECTOR_AUTH_SIN_VERIFICADOR */
} lectura_t;

/* Manda un APDU a la tarjeta apoyada (ISO-DEP: el RATS se hace solo la primera vez). Devuelve el
   largo de la respuesta (con SW1 SW2) o negativo. Solo vale dentro del verificador. */
typedef int (*lector_apdu_fn)(void *ctx, const uint8_t *cmd, size_t n, uint8_t *resp, size_t max);

/* Se llama en la tarea del lector, con la tarjeta en el campo. Devuelve el codigo que va a
   lectura_t.auth. No debe tardar mas de un par de segundos. */
typedef int8_t (*lector_verificador_t)(const uint8_t *uid, uint8_t largo, lector_apdu_fn apdu, void *ctx);
void lector_set_verificador(lector_verificador_t v);

/* Arranca el SPI y el lector. Cada lectura va a `cola` (de elementos lectura_t). */
esp_err_t lector_iniciar(QueueHandle_t cola);

/* Revisa que el RC522 siga vivo y con la antena prendida, y si no, lo reinicia. Llamar cada unos
   segundos desde una tarea que no sea la del lector. Las reglas estan en lector_logica.h. */
lector_salud_t lector_revisar(void);
