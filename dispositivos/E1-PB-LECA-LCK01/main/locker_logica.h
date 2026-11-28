#pragma once
/*
 * Logica del gabinete de lockers (C puro, se prueba en la compu: tests/test_locker.cpp).
 *
 * Una placa, CANT_LOCKERS lockers. Cada locker es una puerta con cerradura por pulso y sensor de
 * apertura (ADR-009), con lo suyo: su cerradura_t (el tope de 5 s y el limite de 20 s por minuto
 * son por bobina, porque cada una se calienta por su cuenta), su sensor, su intrusion, su
 * puerta_abierta, su puerta_cerrada y su gracia de 3 s.
 *
 * Lo del gabinete: UN SOLO PULSO A LA VEZ. La fuente es una, de 5 A, y cada cerradura consume
 * unos 2,2 A (5,4 ohm a 12 V). Un abrir con cualquier cerradura energizada se rechaza con
 * "ocupada", aunque sea para otro locker.
 *
 * Esta logica no toca pines ni la red: recibe lo que pasa (un sensor cambio, llego una orden de
 * abrir, paso el tiempo) y deja en una cola lo que hay que hacer (energizar o cortar la cerradura
 * de un locker, mandar una alerta con su numero, medir). main.c ejecuta esa cola en orden.
 *
 *  - abrir(puerta): puerta de 1 a CANT_LOCKERS (si no, puerta_invalida). Un pulso de pulso_ms. Con
 *    un pulso en curso en el gabinete: ocupada (no se extiende ni se superpone). Si el limite por
 *    minuto de esa cerradura no deja: ocupada, y la alerta limite_pulsos.
 *  - Cortar al abrir (opcional, Kconfig): apenas el sensor marca abierta, se corta el pulso. Sin
 *    energia la cerradura igual deja cerrar la puerta empujando: no hace falta seguir energizando.
 *  - intrusion (critical): un locker se abrio sin pulso en curso y fuera de la gracia que sigue a
 *    su pulso (la traba tarda en volver). Sale ANTES que cualquier otro mensaje del cambio.
 *  - arranque_puerta_abierta (high): la placa arranco con ese locker abierto. No es intrusion (no
 *    se sabe que paso: por ejemplo, un corte de luz y alguien que abrio la cerradura sin energia).
 *  - puerta_abierta (warning): abierto mas de aviso_s (0 = sin aviso).
 *  - puerta_cerrada (info): se cerro despues de cualquiera de las alertas anteriores.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "cerradura_pulso.h"

#define CANT_LOCKERS       4
#define GRACIA_APERTURA_MS 3000
#define ANTIRREBOTE_MS     50
#define ACCIONES_MAX       16

/* pulso_ms y aviso_s de set_config (contrato v4.5, 3.3) */
#define PULSO_DEFECTO_MS   3000
#define AVISO_DEFECTO_S    120
#define AVISO_MIN_S        5               /* 0 = sin aviso; si no, de 5 a 600 */
#define AVISO_MAX_S        600

typedef enum { SEV_INFO, SEV_WARNING, SEV_HIGH, SEV_CRITICAL } severidad_t;

static inline const char *severidad_texto(severidad_t s)
{
    static const char *const t[] = {"info", "warning", "high", "critical"};
    return t[s];
}

typedef enum {
    ACC_PULSO,      /* puerta, valor = ms concedidos */
    ACC_CORTAR,     /* puerta */
    ACC_ALERTA,     /* puerta, tipo, severidad y mensaje */
    ACC_MEDIR,      /* cambio una puerta o una cerradura: medicion en el acto */
} accion_tipo_t;

typedef struct {
    accion_tipo_t tipo;
    uint8_t puerta;              /* 1 a CANT_LOCKERS (0 en ACC_MEDIR) */
    int32_t valor;
    const char *alerta;
    severidad_t severidad;
    const char *mensaje;
} accion_t;

typedef enum { ABRIR_OK, ABRIR_PUERTA_INVALIDA, ABRIR_OCUPADA, ABRIR_LIMITE } abrir_resultado_t;

typedef struct {
    cerradura_t cerradura;
    bool abierta;
    int64_t abierta_desde_ms;
    bool hubo_pulso;
    int64_t fin_pulso_ms;
    bool con_alerta;             /* intrusion o arranque abierto, y todavia no se cerro */
    bool aviso_alertado;         /* ya salio puerta_abierta en esta apertura */
} locker_t;

typedef struct {
    locker_t l[CANT_LOCKERS];
    int32_t pulso_ms;
    int32_t aviso_s;
    bool cortar_al_abrir;
    accion_t cola[ACCIONES_MAX];
    uint8_t cabeza, cantidad;
} gabinete_t;

/* ---------- cola de acciones ---------- */
static inline void g_push(gabinete_t *g, accion_t a)
{
    if (g->cantidad == ACCIONES_MAX) {
        /* No deberia pasar: main la vacia en cada vuelta. Se pierde la mas vieja. */
        g->cabeza = (uint8_t)((g->cabeza + 1) % ACCIONES_MAX);
        g->cantidad--;
    }
    g->cola[(g->cabeza + g->cantidad) % ACCIONES_MAX] = a;
    g->cantidad++;
}

static inline void g_simple(gabinete_t *g, accion_tipo_t tipo, int i, int32_t valor)
{
    accion_t a = {tipo, (uint8_t)(i + 1), valor, NULL, SEV_INFO, NULL};
    g_push(g, a);
}

static inline void g_medir(gabinete_t *g)
{
    accion_t a = {ACC_MEDIR, 0, 0, NULL, SEV_INFO, NULL};
    g_push(g, a);
}

static inline void g_alerta(gabinete_t *g, int i, const char *tipo, severidad_t sev, const char *mensaje)
{
    accion_t a = {ACC_ALERTA, (uint8_t)(i + 1), 0, tipo, sev, mensaje};
    g_push(g, a);
}

static inline bool gabinete_sacar(gabinete_t *g, accion_t *a)
{
    if (g->cantidad == 0) return false;
    *a = g->cola[g->cabeza];
    g->cabeza = (uint8_t)((g->cabeza + 1) % ACCIONES_MAX);
    g->cantidad--;
    return true;
}

/* ---------- estado ---------- */
/* abiertas: el sensor de cada locker al arrancar. Cada locker abierto da arranque_puerta_abierta. */
static inline void gabinete_iniciar(gabinete_t *g, int64_t ahora_ms, const bool abiertas[CANT_LOCKERS],
                                    int32_t pulso_ms, int32_t aviso_s, bool cortar_al_abrir)
{
    memset(g, 0, sizeof(*g));
    g->pulso_ms = pulso_ms;
    g->aviso_s = aviso_s;
    g->cortar_al_abrir = cortar_al_abrir;
    for (int i = 0; i < CANT_LOCKERS; i++) {
        locker_t *l = &g->l[i];
        cerradura_iniciar(&l->cerradura);
        l->abierta = abiertas[i];
        l->abierta_desde_ms = ahora_ms;
        if (abiertas[i]) {
            l->con_alerta = true;
            g_alerta(g, i, "arranque_puerta_abierta", SEV_HIGH, "La placa arranco con el locker abierto");
        }
    }
}

/* true si alguna cerradura del gabinete esta energizada */
static inline bool gabinete_ocupado(const gabinete_t *g)
{
    for (int i = 0; i < CANT_LOCKERS; i++) {
        if (g->l[i].cerradura.energizada) return true;
    }
    return false;
}

static inline void g_cortar(gabinete_t *g, int i, int64_t ahora_ms)
{
    locker_t *l = &g->l[i];
    if (!l->cerradura.energizada) return;
    cerradura_cortar(&l->cerradura, ahora_ms);
    l->hubo_pulso = true;
    l->fin_pulso_ms = ahora_ms;
    g_simple(g, ACC_CORTAR, i, 0);
}

/* Orden de abrir ya validada (vencimiento y hora). puerta: 1 a CANT_LOCKERS. */
static inline abrir_resultado_t gabinete_abrir(gabinete_t *g, int64_t ahora_ms, int puerta)
{
    if (puerta < 1 || puerta > CANT_LOCKERS) return ABRIR_PUERTA_INVALIDA;
    if (gabinete_ocupado(g)) return ABRIR_OCUPADA;              /* un solo pulso en todo el gabinete */
    int i = puerta - 1;
    int64_t concedido = cerradura_pedir(&g->l[i].cerradura, ahora_ms, g->pulso_ms, g->pulso_ms);
    if (concedido <= 0) {
        g_alerta(g, i, "limite_pulsos", SEV_WARNING, "Se llego al limite de tiempo energizada por minuto");
        return ABRIR_LIMITE;
    }
    g_simple(g, ACC_PULSO, i, (int32_t)concedido);
    g_medir(g);
    return ABRIR_OK;
}

/* El sensor ya filtrado (antirrebote) del locker i (0 a CANT_LOCKERS-1) cambio, o no. */
static inline void gabinete_sensor(gabinete_t *g, int i, int64_t ahora_ms, bool abierta)
{
    locker_t *l = &g->l[i];
    if (abierta == l->abierta) return;
    l->abierta = abierta;

    if (abierta) {
        l->abierta_desde_ms = ahora_ms;
        l->aviso_alertado = false;
        bool con_orden = l->cerradura.energizada ||
                         (l->hubo_pulso && (ahora_ms - l->fin_pulso_ms) < GRACIA_APERTURA_MS);
        if (!con_orden) {
            l->con_alerta = true;
            g_alerta(g, i, "intrusion", SEV_CRITICAL, "Locker abierto sin orden de apertura");
        } else if (g->cortar_al_abrir) {
            g_cortar(g, i, ahora_ms);
        }
    } else if (l->con_alerta || l->aviso_alertado) {
        l->con_alerta = false;
        l->aviso_alertado = false;
        g_alerta(g, i, "puerta_cerrada", SEV_INFO, "Locker cerrado");
    }
    g_medir(g);
}

/* El paso del tiempo: vencen los pulsos y los avisos de puerta abierta. */
static inline void gabinete_tick(gabinete_t *g, int64_t ahora_ms)
{
    for (int i = 0; i < CANT_LOCKERS; i++) {
        locker_t *l = &g->l[i];
        if (cerradura_vencio(&l->cerradura, ahora_ms)) {
            g_cortar(g, i, ahora_ms);
            g_medir(g);
        }
        if (l->abierta && g->aviso_s > 0 && !l->aviso_alertado &&
            (ahora_ms - l->abierta_desde_ms) >= (int64_t)g->aviso_s * 1000) {
            l->aviso_alertado = true;
            g_alerta(g, i, "puerta_abierta", SEV_WARNING, "Locker abierto demasiado tiempo");
        }
    }
}

/* ---------- antirrebote de un microswitch ---------- */
typedef struct {
    bool estable;
    bool crudo;
    int64_t desde_ms;
} antirrebote_t;

static inline void antirrebote_iniciar(antirrebote_t *a, bool lectura, int64_t ahora_ms)
{
    a->estable = a->crudo = lectura;
    a->desde_ms = ahora_ms;
}

/* true si el valor estable cambio (quedo ANTIRREBOTE_MS sin moverse). */
static inline bool antirrebote_paso(antirrebote_t *a, bool lectura, int64_t ahora_ms)
{
    if (lectura != a->crudo) {
        a->crudo = lectura;
        a->desde_ms = ahora_ms;
        return false;
    }
    if (a->crudo != a->estable && (ahora_ms - a->desde_ms) >= ANTIRREBOTE_MS) {
        a->estable = a->crudo;
        return true;
    }
    return false;
}

/* ---------- vigencia de la orden de abrir (igual que _modelos/PUE-RFID) ----------
 * `abrir` llega con un vencimiento (epoch en segundos). Se rechaza sin hora valida (no se puede
 * saber si vencio), vencida, o con un vencimiento a mas de 300 s: una orden que quedo encolada
 * en el broker no se tiene que poder ejecutar horas despues. */
#define HORA_VALIDA_DESDE_S 1735689600LL    /* 2025-01-01 */
#define ORDEN_VIGENCIA_MAX_S 300

typedef enum {
    ORDEN_OK = 0,
    ORDEN_SIN_VENCIMIENTO,
    ORDEN_SIN_HORA,
    ORDEN_VENCIDA,
    ORDEN_VENCIMIENTO_LEJANO
} orden_estado_t;

static inline orden_estado_t orden_vigente(bool tiene_vence, int64_t vence_s, int64_t ahora_s)
{
    if (!tiene_vence) return ORDEN_SIN_VENCIMIENTO;
    if (ahora_s < HORA_VALIDA_DESDE_S) return ORDEN_SIN_HORA;
    if (vence_s <= ahora_s) return ORDEN_VENCIDA;
    if (vence_s - ahora_s > ORDEN_VIGENCIA_MAX_S) return ORDEN_VENCIMIENTO_LEJANO;
    return ORDEN_OK;
}

static inline const char *orden_motivo(orden_estado_t o)
{
    static const char *const m[] = {"", "sin_vencimiento", "sin_hora_valida", "orden_vencida",
                                    "vencimiento_muy_lejano"};
    return m[o];
}

/* params.puerta de abrir: un entero de 1 a CANT_LOCKERS. tiene = vino como numero. */
static inline bool puerta_valida(bool tiene, double v, int *puerta)
{
    if (!tiene || !(v >= 1 && v <= CANT_LOCKERS)) return false;      /* tambien descarta NaN */
    int p = (int)v;
    if ((double)p != v) return false;                                /* 1.5 no es una puerta */
    *puerta = p;
    return true;
}

/* aviso_s: 0 (sin aviso) o de AVISO_MIN_S a AVISO_MAX_S */
static inline bool aviso_valido(int32_t v)
{
    return v == 0 || (v >= AVISO_MIN_S && v <= AVISO_MAX_S);
}
