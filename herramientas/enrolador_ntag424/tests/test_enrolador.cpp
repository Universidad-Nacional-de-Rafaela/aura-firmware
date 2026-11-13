#include "../enrolador_logica.h"
#include "../../../comun/sha256_hmac.h"
#include "../../../comun/tests/aserciones.h"
#include "../../../comun/tests/tarjeta_sim.h"

struct Servicios {
    uint8_t maestra[32];
    bool derivar_ok = true;
    int derivaciones = 0;
};
struct Ctx { Servicios *sv; Tarjeta *t; };

static int apdu_c(void *c, const uint8_t *cmd, size_t n, uint8_t *r, size_t m) { return tarjeta_apdu(((Ctx *)c)->t, cmd, n, r, m); }
static bool derivar_c(void *c, const uint8_t *u, size_t l, uint8_t *o)
{
    Servicios *s = ((Ctx *)c)->sv;
    s->derivaciones++;
    return s->derivar_ok && clave_de_tarjeta(s->maestra, u, l, o) == 0;
}
static bool azar_c(void *, uint8_t *o) { for (int i = 0; i < 16; i++) o[i] = (uint8_t)(0xA0 + 5 * i); return true; }

static const uint8_t UID[7] = {0x04, 0x45, 0x65, 0xBA, 0x77, 0x23, 0x90};

static void derivada(const Servicios &sv, uint8_t out[16]) { clave_de_tarjeta(sv.maestra, UID, 7, out); }

static void preparar(Servicios *sv, Tarjeta *t)    /* tarjeta nueva: clave de fabrica */
{
    for (int i = 0; i < 32; i++) sv->maestra[i] = (uint8_t)(0x10 + i);
    memcpy(t->uid, UID, 7);
    memset(t->clave, 0, 16);
}

static bool tiene_clave(const Tarjeta &t, const uint8_t k[16]) { return memcmp(t.clave, k, 16) == 0; }

#define OPS(sv, t) Ctx c = {&sv, &t}; enr_ops_t ops = {apdu_c, derivar_c, azar_c, &c}

int main()
{
    uint8_t k[16], cero[16] = {0};

    {   /* enrolar una tarjeta nueva */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        derivada(sv, k);
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_ENROLADA_OK);
        VERIFICAR(tiene_clave(t, k));
        VERIFICAR(t.cambios == 1);
        VERIFICAR(t.auth_rechazadas == 0);              /* una tarjeta nueva no gasta intentos fallidos */
        VERIFICAR(enr_estado(&ops, UID, 7) == ENR_ESTADO_ENROLADA);
        /* idempotente: enrolar de nuevo no cambia nada */
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_YA_ENROLADA);
        VERIFICAR(t.cambios == 1);
        VERIFICAR(t.auth_rechazadas == 1);              /* solo el intento con la clave de fabrica */
    }
    {   /* el estado de una tarjeta nueva y la de una enrolada */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        VERIFICAR(enr_estado(&ops, UID, 7) == ENR_ESTADO_DE_FABRICA);
        VERIFICAR(t.cambios == 0);
        VERIFICAR(tiene_clave(t, cero));
    }
    {   /* otra clave desconocida (otra maestra, otro sistema): no se la toca */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        for (int i = 0; i < 16; i++) t.clave[i] = (uint8_t)(0x77 + i);
        uint8_t antes[16];
        memcpy(antes, t.clave, 16);
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_CLAVE_DESCONOCIDA);
        VERIFICAR(t.cambios == 0);
        VERIFICAR(tiene_clave(t, antes));
        VERIFICAR(enr_estado(&ops, UID, 7) == ENR_CLAVE_DESCONOCIDA);
        VERIFICAR(enr_desenrolar(&ops, UID, 7) == ENR_CLAVE_DESCONOCIDA);
        VERIFICAR(t.cambios == 0);
    }
    {   /* la tarjeta rechaza el cambio: sigue con la clave de fabrica */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        t.rechazar_cambio = true;
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_CAMBIO_RECHAZADO);
        VERIFICAR(tiene_clave(t, cero));
        VERIFICAR(enr_estado(&ops, UID, 7) == ENR_ESTADO_DE_FABRICA);
    }
    {   /* dice que si y no cambia: se detecta porque se comprueba con la clave nueva */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        t.ignorar_cambio = true;
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_SIN_CAMBIO);
        VERIFICAR(tiene_clave(t, cero));
    }
    {   /* aplica el cambio pero la respuesta no llega: igual se la da por enrolada */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        derivada(sv, k);
        t.perder_ack_cambio = true;
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_ENROLADA_OK);
        VERIFICAR(tiene_clave(t, k));
    }
    {   /* aplica el cambio y se corta todo: no se sabe en que quedo */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        t.oscura_tras_cambio = true;
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_INCIERTA);
        t.oscura = false;                                /* la vuelven a apoyar */
        VERIFICAR(enr_estado(&ops, UID, 7) == ENR_ESTADO_ENROLADA);
    }
    {   /* la tarjeta queda con una clave que no es ni la vieja ni la nueva */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        t.aplicar_otra_clave = true;
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_POSIBLEMENTE_PERDIDA);
        VERIFICAR(enr_estado(&ops, UID, 7) == ENR_CLAVE_DESCONOCIDA);
    }
    for (int falla = 0; falla < 4; falla++) {            /* se corta ANTES de tocarla: sin riesgo */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        t.callar_en = falla;
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_ERR_COMUNICACION);
        VERIFICAR(t.cambios == 0);
        VERIFICAR(tiene_clave(t, cero));
    }
    {   /* se corta justo el ChangeKey (antes de que lo procese): se sabe que sigue igual */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        t.callar_en = 4;
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_SIN_CAMBIO);
        VERIFICAR(tiene_clave(t, cero));
    }
    {   /* no es una NTAG 424 DNA */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        t.sin_aplicacion = true;
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_ERR_SELECCION);
        VERIFICAR(t.cambios == 0);
    }
    {   /* el UID del aire no es el real: no se cambia nada */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        t.uid_falso_en_canal = true;
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_ERR_PROTOCOLO);
        VERIFICAR(t.cambios == 0);
    }
    {   /* UID de largo invalido y falla de derivacion: no se le habla a la tarjeta */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        const uint8_t u4[4] = {1, 2, 3, 4};
        VERIFICAR(enr_enrolar(&ops, u4, 4) == ENR_UID_INVALIDO);
        VERIFICAR(enr_desenrolar(&ops, u4, 4) == ENR_UID_INVALIDO);
        VERIFICAR(enr_estado(&ops, u4, 4) == ENR_UID_INVALIDO);
        sv.derivar_ok = false;
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_ERR_SERVICIO);
        VERIFICAR(enr_estado(&ops, UID, 7) == ENR_ERR_SERVICIO);
        VERIFICAR(t.apdus == 0);
    }
    {   /* desenrolar: vuelve a la clave de fabrica y se puede reutilizar */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_ENROLADA_OK);
        VERIFICAR(enr_desenrolar(&ops, UID, 7) == ENR_DESENROLADA_OK);
        VERIFICAR(tiene_clave(t, cero));
        VERIFICAR(enr_desenrolar(&ops, UID, 7) == ENR_YA_DE_FABRICA);
        VERIFICAR(t.cambios == 2);                       /* uno para enrolar y uno para desenrolar */
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_ENROLADA_OK);   /* y se vuelve a enrolar */
    }
    {   /* desenrolar una tarjeta nueva no le cambia nada */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        VERIFICAR(enr_desenrolar(&ops, UID, 7) == ENR_YA_DE_FABRICA);
        VERIFICAR(t.cambios == 0);
    }
    {   /* otra maestra: lo enrolado con una no se desenrola con otra */
        Servicios sv; Tarjeta t; preparar(&sv, &t); OPS(sv, t);
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_ENROLADA_OK);
        sv.maestra[0] ^= 1;
        VERIFICAR(enr_desenrolar(&ops, UID, 7) == ENR_CLAVE_DESCONOCIDA);
        VERIFICAR(enr_enrolar(&ops, UID, 7) == ENR_CLAVE_DESCONOCIDA);
        VERIFICAR(t.cambios == 1);
    }
    {   /* los textos */
        VERIFICAR(strcmp(enr_texto(ENR_ENROLADA_OK), "enrolada") == 0);
        VERIFICAR(strcmp(enr_texto(ENR_POSIBLEMENTE_PERDIDA), "posiblemente_perdida") == 0);
        VERIFICAR(strcmp(enr_texto(ENR_INCIERTA), "incierta") == 0);
    }
    RESUMEN();
}
