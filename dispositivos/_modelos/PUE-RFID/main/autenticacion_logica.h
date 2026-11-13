#pragma once
/*
 * Autenticacion de una tarjeta NTAG 424 DNA en la puerta (C puro, sin ESP-IDF: se prueba en la
 * compu).
 *
 * Reemplaza "abre porque el UID esta en la lista" por "abre porque la tarjeta demostro conocer su
 * clave". El UID se puede copiar con un celular; la clave AES de la tarjeta no sale del chip.
 *
 * Orden (cada paso solo si el anterior salio bien):
 *   1. el UID tiene que estar en la lista que manda AURA. Si no, NO se le habla a la tarjeta
 *      (asi una tarjeta ajena no gasta intentos de autenticacion);
 *   2. se deriva la clave de ESA tarjeta a partir de su UID (HMAC con la clave maestra);
 *   3. SELECT de la aplicacion y AuthenticateEV2First con esa clave (comun/ntag424.h);
 *   4. GetCardUID por el canal seguro: el UID real de la tarjeta tiene que ser el que se leyo por
 *      el aire, que se puede falsificar.
 *
 * Lo que depende del hardware entra por ntag_ops_t: hablar con la tarjeta, derivar la clave (en la
 * placa lo hace el HMAC del eFuse, la clave maestra no sale del chip) y el azar. Asi la logica se
 * prueba entera en la compu con una tarjeta simulada.
 *
 * Solo sirven tarjetas de UID de 7 bytes (el de la NTAG 424 DNA). Una tarjeta con "ID aleatorio"
 * activado cambia de UID en cada lectura y no se puede enrolar con este esquema.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lista_tarjetas.h"
#include "../../../../comun/ntag424.h"

#define AUTH_UID_BYTES NTAG_UID_BYTES

typedef ntag_ops_t auth_ops_t;

typedef enum {
    AUTH_OK = 0,                /* la tarjeta conoce su clave y su UID es el leido */
    AUTH_NO_AUTORIZADA,         /* el UID no esta en la lista (no se le hablo a la tarjeta) */
    AUTH_UID_NO_SOPORTADO,      /* en la lista pero no mide 7 bytes: no se puede autenticar */
    AUTH_ERR_SERVICIO,          /* no se pudo derivar la clave o conseguir azar (nada a la tarjeta) */
    AUTH_ERR_COMUNICACION,      /* la tarjeta no contesto: se la sacaron o hay mala lectura */
    AUTH_ERR_SELECCION,         /* no tiene la aplicacion de la NTAG 424 DNA */
    AUTH_CLAVE_RECHAZADA,       /* la tarjeta dijo que la clave no es la suya (de fabrica, de otra
                                   maestra, o un clon del UID) */
    AUTH_ERR_PROTOCOLO,         /* contesto algo que una tarjeta de verdad no contesta */
    AUTH_UID_NO_COINCIDE        /* se autentico pero su UID real no es el que se leyo por el aire */
} auth_resultado_t;

static inline auth_resultado_t auth_desde_ntag(ntag_resultado_t r)
{
    switch (r) {
    case NTAG_OK: return AUTH_OK;
    case NTAG_ERR_AZAR: return AUTH_ERR_SERVICIO;
    case NTAG_ERR_COMUNICACION: return AUTH_ERR_COMUNICACION;
    case NTAG_ERR_SELECCION: return AUTH_ERR_SELECCION;
    case NTAG_CLAVE_RECHAZADA: return AUTH_CLAVE_RECHAZADA;
    case NTAG_UID_NO_COINCIDE: return AUTH_UID_NO_COINCIDE;
    case NTAG_ERR_PROTOCOLO: break;
    }
    return AUTH_ERR_PROTOCOLO;
}

static inline auth_resultado_t auth_verificar(const lista_t *lista, const uint8_t *uid, size_t largo,
                                              const auth_ops_t *ops)
{
    if (!lista_autorizada(lista, uid, largo)) return AUTH_NO_AUTORIZADA;
    if (largo != AUTH_UID_BYTES) return AUTH_UID_NO_SOPORTADO;

    uint8_t clave[16];
    ev2_sesion s;
    if (!ops->clave_de_tarjeta(ops->ctx, uid, largo, clave)) return AUTH_ERR_SERVICIO;

    ntag_resultado_t r = ntag_autenticar(ops, clave, &s);
    if (r == NTAG_OK) r = ntag_confirmar_uid(ops, &s, uid);

    ntag_borrar(clave, sizeof(clave));
    ntag_borrar(&s, sizeof(s));
    return auth_desde_ntag(r);
}

static inline const char *auth_texto(auth_resultado_t r)
{
    switch (r) {
    case AUTH_OK: return "ok";
    case AUTH_NO_AUTORIZADA: return "no_autorizada";
    case AUTH_UID_NO_SOPORTADO: return "uid_no_soportado";
    case AUTH_ERR_SERVICIO: return "error_servicio";
    case AUTH_ERR_COMUNICACION: return "error_comunicacion";
    case AUTH_ERR_SELECCION: return "error_seleccion";
    case AUTH_CLAVE_RECHAZADA: return "clave_rechazada";
    case AUTH_ERR_PROTOCOLO: return "error_protocolo";
    case AUTH_UID_NO_COINCIDE: return "uid_no_coincide";
    }
    return "desconocido";
}
