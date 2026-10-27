/*
 * Actualización por la malla (OTA): tramas entre el gateway y un nodo (hoja o concentrador).
 *
 * Es interna de la mesh, como LOTE: no entra en una trama común (180 B), así que tiene su propia
 * cabecera. El gateway descarga la imagen una vez y se la pasa a un nodo por vez, en bloques que
 * PIDE el nodo: el nodo dice desde qué byte quiere seguir y el gateway le manda una ventana de
 * bloques. Si se pierde un bloque, el nodo lo vuelve a pedir; si se corta todo, lo dice el que
 * espera (el nodo a los AURA_OTA_ESPERA_MS, el gateway a los AURA_OTA_CORTE_MS).
 *
 *   trama OTA:  versión(1)=3  tipo(1)=OTA(14)  op(1)  sesión(4, LE)  cuerpo
 *
 *   gateway -> nodo   OFERTA     tamaño(4) chip(2) forzar(1) sha256(32) tipo(32) versión(32)
 *   nodo -> gateway   PIDO       desde(4): el byte desde el que quiere seguir (0 = acepta)
 *   gateway -> nodo   DATOS      desde(4) bytes(<= AURA_OTA_BLOQUE)
 *   nodo -> gateway   FIN        resultado(1) motivo(texto, <= 40)
 *   gateway -> nodo   CANCELAR   (sin cuerpo)
 *   gateway -> nodo   CONFIRMADA (sin cuerpo): volviste con la versión nueva; confirmala (no vuelve
 *                     atrás). No depende del broker: el gateway lo manda al ver el reporte del nodo.
 *
 * El nodo manda lo suyo a la raíz (esp_mesh_send con destino NULL): no pasa por la lógica del
 * concentrador, que solo reenvía.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "aura_trama.h"

#define AURA_TIPO_OTA           14

enum {
    AURA_OTA_OFERTA   = 1,
    AURA_OTA_PIDO     = 2,
    AURA_OTA_DATOS    = 3,
    AURA_OTA_FIN      = 4,
    AURA_OTA_CANCELAR = 5,
    AURA_OTA_CONFIRMADA = 6,
};

/* Resultado del FIN */
enum {
    AURA_OTA_LISTO    = 0,    /* imagen completa y verificada: el nodo se reinicia con ella */
    AURA_OTA_YA_TIENE = 1,    /* el nodo ya corre esa versión de ese tipo: no hace nada */
    AURA_OTA_RECHAZO  = 2,    /* no la aplica; el motivo va en el texto */
};

#define AURA_OTA_CABECERA       7
#define AURA_OTA_BLOQUE         1400    /* cabecera + desde + bloque = 1411 < MESH_MPS (1472) */
#define AURA_OTA_TRAMA_MAX      (AURA_OTA_CABECERA + 4 + AURA_OTA_BLOQUE)
#define AURA_OTA_VENTANA        4       /* bloques por pedido */
#define AURA_OTA_TEXTO          32      /* tipo y versión: como en esp_app_desc_t */
#define AURA_OTA_MOTIVO_MAX     40
#define AURA_OTA_ESPERA_MS      3000    /* nodo: sin datos tanto -> vuelve a pedir */
#define AURA_OTA_PEDIDOS_MAX    10      /* nodo: tantos pedidos seguidos sin respuesta -> abandona */
#define AURA_OTA_CORTE_MS       30000   /* gateway: el nodo no pide nada tanto -> transferencia cortada */

typedef struct __attribute__((packed)) {
    uint32_t tamano;
    uint16_t chip;                      /* esp_chip_id_t de la imagen: un C3 no acepta una de S3 */
    uint8_t  forzar;                    /* 1 = aunque el nodo sea de otro tipo (cambio de uso) */
    uint8_t  sha256[32];
    char     tipo[AURA_OTA_TEXTO];      /* project_name de la imagen */
    char     version[AURA_OTA_TEXTO];   /* version de la imagen */
} aura_ota_oferta_t;

static inline bool aura_es_ota(const uint8_t *d, size_t len)
{
    return len >= AURA_OTA_CABECERA && len <= AURA_OTA_TRAMA_MAX &&
           d[0] == AURA_PROTO_VERSION && d[1] == AURA_TIPO_OTA;
}

/* Arma la cabecera; devuelve dónde empieza el cuerpo */
static inline uint8_t *aura_ota_cabecera(uint8_t *buf, uint8_t op, uint32_t sesion)
{
    buf[0] = AURA_PROTO_VERSION;
    buf[1] = AURA_TIPO_OTA;
    buf[2] = op;
    aura_u32_escribir(buf + 3, sesion);
    return buf + AURA_OTA_CABECERA;
}

static inline uint8_t aura_ota_op(const uint8_t *d)
{
    return d[2];
}

static inline uint32_t aura_ota_sesion(const uint8_t *d)
{
    return aura_u32_leer(d + 3);
}

/* ===== Tipo y versión del firmware (regla en GUIA_NODOS_HOJA.md, "Tipo y versión") ===== */
/* Tipo: "aura-" y después a-z, 0-9 y "-", hasta 31 caracteres en total (project_name). */
static inline bool aura_tipo_valido(const char *t)
{
    size_t n = t ? strnlen(t, AURA_OTA_TEXTO) : 0;
    if (n <= 5 || n >= AURA_OTA_TEXTO || strncmp(t, "aura-", 5) != 0 || t[n - 1] == '-') {
        return false;
    }
    for (size_t i = 5; i < n; i++) {
        char c = t[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

/* Versión: mayor.menor.parche, solo números (p. ej. "1.3.0") */
static inline bool aura_version_valida(const char *v)
{
    int puntos = 0, digitos = 0;
    size_t n = v ? strnlen(v, AURA_OTA_TEXTO) : 0;
    if (n == 0 || n >= AURA_OTA_TEXTO) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (v[i] == '.') {
            if (digitos == 0) {
                return false;
            }
            puntos++;
            digitos = 0;
        } else if (v[i] >= '0' && v[i] <= '9') {
            digitos++;
        } else {
            return false;
        }
    }
    return puntos == 2 && digitos > 0;
}
