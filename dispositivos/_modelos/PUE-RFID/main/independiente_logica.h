#pragma once
/*
 * Modo independiente (C puro, sin ESP-IDF: se prueba en la compu). ADR-009, "Etapa transitoria:
 * puertas sin conexion".
 *
 * La puerta no se une a la mesh: la lista de llaveros se compila en la imagen y de la placa no sale
 * ningun dato. La cerradura la maneja la MISMA maquina de estados que la puerta conectada
 * (puerta_logica.h): el pulso, su tope, el limite por minuto, el bloqueo por rechazos y el aviso
 * sonoro son los de siempre. Aca solo se le recorta lo que este modo no tiene:
 *   - armado: despues de cada llavero se cierra la ventana de la doble pasada, asi que nunca se
 *     arma y por lo tanto nunca hay intrusion;
 *   - registro, alertas, reportes y NVS: esas acciones se descartan; main_independiente.c solo
 *     recibe las de la cerradura y las del buzzer.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lista_tarjetas.h"
#include "puerta_logica.h"

/* Con la puerta abierta mas de este tiempo, aviso en la placa. Es fijo: en este modo no hay
   set_config. El sonido y su tope (TOPE_AVISO_MS) son los de la puerta conectada. */
#define INDEPENDIENTE_AVISO_S AVISO_DEFECTO_S

/* ---------- Lista compilada ---------- */
/* Carga en `l` los UID del archivo de llaveros y devuelve cuantas entradas no sirven (mal
   escritas, repetidas o que no entran). Las demas se cargan igual, como al leer de NVS.
   *primera_mala queda con el numero (desde 1) de la primera que no sirve, o 0 si sirven todas. */
static inline size_t independiente_cargar_lista(lista_t *l, const char *const *uids, size_t n, size_t *primera_mala)
{
    lista_vaciar(l);
    *primera_mala = 0;
    size_t malas = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t uid[UID_MAX_BYTES];
        uint8_t largo;
        if (!hex_a_uid(uids[i], uid, &largo) || lista_agregar(l, uid, largo) != LISTA_OK) {
            if (malas == 0) *primera_mala = i + 1;
            malas++;
        }
    }
    return malas;
}

/* ---------- La puerta ---------- */
/* Arranca siempre desarmada y con los tiempos fijos, sin mirar lo que haya guardado la placa */
static inline void independiente_iniciar(puerta_t *p, int64_t ahora_ms, bool abierta)
{
    puerta_iniciar(p, ahora_ms, false, abierta, PULSO_DEFECTO_MS, INDEPENDIENTE_AVISO_S);
}

static inline void independiente_tarjeta(puerta_t *p, int64_t ahora_ms, const uint8_t *uid, uint8_t largo, bool autorizada)
{
    puerta_tarjeta(p, ahora_ms, uid, largo, autorizada);
    p->doble_activa = false;           /* sin armado: la pasada siguiente es otro acceso */
}

/* Solo la cerradura y el buzzer */
static inline bool independiente_accion_local(accion_tipo_t tipo)
{
    return tipo == ACC_PULSO || tipo == ACC_CORTAR_PULSO || tipo == ACC_SONIDO || tipo == ACC_AVISO;
}

/* Como puerta_sacar, pero descarta lo que saldria de la placa o se guardaria. false si no hay. */
static inline bool independiente_sacar(puerta_t *p, accion_t *out)
{
    while (puerta_sacar(p, out)) {
        if (independiente_accion_local(out->tipo)) return true;
    }
    return false;
}
