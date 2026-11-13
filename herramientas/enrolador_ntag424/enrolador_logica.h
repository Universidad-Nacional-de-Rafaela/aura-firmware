#pragma once
/*
 * Logica de la estacion de enrolamiento de NTAG 424 DNA (C puro, sin ESP-IDF: se prueba en la
 * compu). Es lo unico del sistema que CAMBIA la clave de una tarjeta; las puertas solo
 * autentican.
 *
 * Una tarjeta nueva trae la clave 0 en ceros ("de fabrica"). Enrolarla es cambiarle la clave 0 por
 * la que sale de HMAC(clave maestra, UID) (la misma cuenta que hace la puerta). Desenrolarla es
 * devolverle ceros, para reutilizar el sticker. Se necesita la clave maestra para las dos cosas.
 *
 * LA PRIMERA REGLA: no dejar una tarjeta con una clave que no se puede reproducir. Una NTAG 424
 * DNA sin su clave 0 no se puede reconfigurar nunca mas. Por eso cada cambio:
 *   - se hace solo con una sesion ya autenticada con la clave actual (nunca "a ciegas");
 *   - se COMPRUEBA autenticando con la clave nueva: el "91 00" de la tarjeta no alcanza;
 *   - si la comprobacion falla, se vuelve a probar la clave vieja para saber en que quedo.
 * Y es idempotente: si la tarjeta ya esta en el estado pedido no se le cambia nada.
 *
 * Los cambios son ENR_*_OK (hecho y comprobado) o uno de los demas resultados, que dicen en que
 * estado quedo la tarjeta. Los dos que importan: ENR_INCIERTA (se cortó la comunicacion en medio:
 * volver a apoyarla y mirar el estado) y ENR_POSIBLEMENTE_PERDIDA (ni la clave vieja ni la nueva
 * la autentican: anotar el UID y no tocarla mas).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../../comun/ntag424.h"

#define ENR_VERSION_CLAVE 0x01     /* version que se anota en la clave nueva (de fabrica es 0x00) */

typedef ntag_ops_t enr_ops_t;

typedef enum {
    ENR_ENROLADA_OK = 0,       /* se le cambio la clave y la clave nueva la autentica */
    ENR_YA_ENROLADA,           /* ya tenia la clave derivada: no se le cambio nada */
    ENR_DESENROLADA_OK,        /* volvio a la clave de fabrica y la de fabrica la autentica */
    ENR_YA_DE_FABRICA,         /* ya tenia la clave de fabrica: no se le cambio nada */
    ENR_ESTADO_ENROLADA,       /* (solo en enr_estado) tiene la clave derivada */
    ENR_ESTADO_DE_FABRICA,     /* (solo en enr_estado) tiene la clave de fabrica */
    ENR_UID_INVALIDO,          /* no mide 7 bytes */
    ENR_ERR_SERVICIO,          /* no se pudo derivar la clave o conseguir azar (nada a la tarjeta) */
    ENR_ERR_COMUNICACION,      /* no contesto antes de tocarla: se puede reintentar sin riesgo */
    ENR_ERR_SELECCION,         /* no es una NTAG 424 DNA */
    ENR_ERR_PROTOCOLO,         /* contesto algo que una tarjeta de verdad no contesta */
    ENR_CLAVE_DESCONOCIDA,     /* ni la de fabrica ni la derivada: otra maestra u otro sistema. No se toco */
    ENR_CAMBIO_RECHAZADO,      /* la tarjeta rechazo el cambio y sigue con la clave vieja */
    ENR_SIN_CAMBIO,            /* dijo que si pero sigue con la clave vieja */
    ENR_INCIERTA,              /* se corto la comunicacion en medio del cambio: no se sabe, volver a mirar */
    ENR_POSIBLEMENTE_PERDIDA   /* tras el cambio no la autentica ni la clave vieja ni la nueva */
} enr_resultado_t;

static const uint8_t ENR_CLAVE_DE_FABRICA[16] = {0};

static inline enr_resultado_t enr_desde_ntag(ntag_resultado_t r)
{
    switch (r) {
    case NTAG_ERR_AZAR: return ENR_ERR_SERVICIO;
    case NTAG_ERR_COMUNICACION: return ENR_ERR_COMUNICACION;
    case NTAG_ERR_SELECCION: return ENR_ERR_SELECCION;
    default: break;
    }
    return ENR_ERR_PROTOCOLO;
}

/* Autentica con `clave` y confirma que el UID del canal seguro es `uid`. Deja la sesion en *s. */
static inline ntag_resultado_t enr_probar(const enr_ops_t *ops, const uint8_t clave[16], const uint8_t *uid,
                                          ev2_sesion *s)
{
    ntag_resultado_t r = ntag_autenticar(ops, clave, s);
    if (r == NTAG_OK) r = ntag_confirmar_uid(ops, s, uid);
    return r;
}

/* Que clave tiene la tarjeta: gasta como mucho dos intentos de autenticacion. No cambia nada. */
static inline enr_resultado_t enr_estado(const enr_ops_t *ops, const uint8_t *uid, size_t largo)
{
    if (largo != NTAG_UID_BYTES) return ENR_UID_INVALIDO;
    uint8_t derivada[16];
    ev2_sesion s;
    enr_resultado_t res;
    if (!ops->clave_de_tarjeta(ops->ctx, uid, largo, derivada)) return ENR_ERR_SERVICIO;

    ntag_resultado_t r = enr_probar(ops, derivada, uid, &s);
    if (r == NTAG_OK) {
        res = ENR_ESTADO_ENROLADA;
    } else if (r == NTAG_CLAVE_RECHAZADA) {
        r = enr_probar(ops, ENR_CLAVE_DE_FABRICA, uid, &s);
        if (r == NTAG_OK) res = ENR_ESTADO_DE_FABRICA;
        else if (r == NTAG_CLAVE_RECHAZADA) res = ENR_CLAVE_DESCONOCIDA;
        else res = enr_desde_ntag(r);
    } else {
        res = enr_desde_ntag(r);
    }
    ntag_borrar(derivada, sizeof(derivada));
    ntag_borrar(&s, sizeof(s));
    return res;
}

/* Cambia la clave 0 de `desde` a `hacia` (las dos de 16 bytes) y comprueba el resultado. */
static inline enr_resultado_t enr_transicion(const enr_ops_t *ops, const uint8_t *uid, const uint8_t desde[16],
                                             const uint8_t hacia[16], uint8_t version,
                                             enr_resultado_t si_salio_bien, enr_resultado_t si_ya_estaba)
{
    ev2_sesion s;
    enr_resultado_t res;

    ntag_resultado_t r = enr_probar(ops, desde, uid, &s);
    if (r == NTAG_CLAVE_RECHAZADA) {
        /* no tiene la clave de partida: a lo mejor ya esta en la de llegada */
        r = enr_probar(ops, hacia, uid, &s);
        if (r == NTAG_OK) res = si_ya_estaba;
        else if (r == NTAG_CLAVE_RECHAZADA) res = ENR_CLAVE_DESCONOCIDA;
        else res = enr_desde_ntag(r);
        goto fin;
    }
    if (r != NTAG_OK) { res = enr_desde_ntag(r); goto fin; }

    {
        ntag_resultado_t cambio = ntag_cambiar_clave0(ops, &s, hacia, version);

        /* Comprobar siempre con la clave nueva, aunque el cambio haya contestado mal o no haya
           contestado: puede haberse aplicado igual. */
        r = enr_probar(ops, hacia, uid, &s);
        if (r == NTAG_OK) { res = si_salio_bien; goto fin; }
        if (r == NTAG_CLAVE_RECHAZADA) {
            r = enr_probar(ops, desde, uid, &s);
            if (r == NTAG_OK) res = (cambio == NTAG_ERR_PROTOCOLO) ? ENR_CAMBIO_RECHAZADO : ENR_SIN_CAMBIO;
            else if (r == NTAG_CLAVE_RECHAZADA) res = ENR_POSIBLEMENTE_PERDIDA;
            else res = ENR_INCIERTA;
        } else {
            res = ENR_INCIERTA;       /* no se pudo comprobar: la tarjeta se fue */
        }
    }
fin:
    ntag_borrar(&s, sizeof(s));
    return res;
}

static inline enr_resultado_t enr_enrolar(const enr_ops_t *ops, const uint8_t *uid, size_t largo)
{
    if (largo != NTAG_UID_BYTES) return ENR_UID_INVALIDO;
    uint8_t derivada[16];
    if (!ops->clave_de_tarjeta(ops->ctx, uid, largo, derivada)) return ENR_ERR_SERVICIO;
    enr_resultado_t res = enr_transicion(ops, uid, ENR_CLAVE_DE_FABRICA, derivada, ENR_VERSION_CLAVE,
                                         ENR_ENROLADA_OK, ENR_YA_ENROLADA);
    ntag_borrar(derivada, sizeof(derivada));
    return res;
}

static inline enr_resultado_t enr_desenrolar(const enr_ops_t *ops, const uint8_t *uid, size_t largo)
{
    if (largo != NTAG_UID_BYTES) return ENR_UID_INVALIDO;
    uint8_t derivada[16];
    if (!ops->clave_de_tarjeta(ops->ctx, uid, largo, derivada)) return ENR_ERR_SERVICIO;
    enr_resultado_t res = enr_transicion(ops, uid, derivada, ENR_CLAVE_DE_FABRICA, 0x00,
                                         ENR_DESENROLADA_OK, ENR_YA_DE_FABRICA);
    ntag_borrar(derivada, sizeof(derivada));
    return res;
}

static inline const char *enr_texto(enr_resultado_t r)
{
    switch (r) {
    case ENR_ENROLADA_OK: return "enrolada";
    case ENR_YA_ENROLADA: return "ya_enrolada";
    case ENR_DESENROLADA_OK: return "desenrolada";
    case ENR_YA_DE_FABRICA: return "ya_de_fabrica";
    case ENR_ESTADO_ENROLADA: return "enrolada";
    case ENR_ESTADO_DE_FABRICA: return "de_fabrica";
    case ENR_UID_INVALIDO: return "uid_invalido";
    case ENR_ERR_SERVICIO: return "error_servicio";
    case ENR_ERR_COMUNICACION: return "error_comunicacion";
    case ENR_ERR_SELECCION: return "no_es_ntag424";
    case ENR_ERR_PROTOCOLO: return "error_protocolo";
    case ENR_CLAVE_DESCONOCIDA: return "clave_desconocida";
    case ENR_CAMBIO_RECHAZADO: return "cambio_rechazado";
    case ENR_SIN_CAMBIO: return "sin_cambio";
    case ENR_INCIERTA: return "incierta";
    case ENR_POSIBLEMENTE_PERDIDA: return "posiblemente_perdida";
    }
    return "desconocido";
}
