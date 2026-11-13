#include "../main/autenticacion_logica.h"
#include "../../../../comun/sha256_hmac.h"
#include "../../../../comun/tests/aserciones.h"
#include "../../../../comun/tests/tarjeta_sim.h"

/* Derivacion como en banco: HMAC-SHA256(maestra, UID), 16 bytes. En la placa lo hace el eFuse. */
struct Servicios {
    uint8_t maestra[32];
    bool derivar_ok = true;
    bool azar_ok = true;
    int derivaciones = 0;
};

static bool derivar(void *ctx, const uint8_t *uid, size_t largo, uint8_t out[16])
{
    Servicios *s = (Servicios *)ctx;
    s->derivaciones++;
    if (!s->derivar_ok) return false;
    return clave_de_tarjeta(s->maestra, uid, largo, out) == 0;
}

static bool azar(void *ctx, uint8_t out[16])
{
    Servicios *s = (Servicios *)ctx;
    if (!s->azar_ok) return false;
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(0xA0 + 5 * i);
    return true;
}

/* Un unico ctx que lleva las dos cosas */
struct Ctx { Servicios *sv; Tarjeta *t; };
static int apdu_c(void *c, const uint8_t *cmd, size_t n, uint8_t *r, size_t m) { return tarjeta_apdu(((Ctx *)c)->t, cmd, n, r, m); }
static bool derivar_c(void *c, const uint8_t *u, size_t l, uint8_t *o) { return derivar(((Ctx *)c)->sv, u, l, o); }
static bool azar_c(void *c, uint8_t *o) { return azar(((Ctx *)c)->sv, o); }

static const uint8_t UID[7] = {0x04, 0x45, 0x65, 0xBA, 0x77, 0x23, 0x90};

static void preparar(Servicios *sv, Tarjeta *t, lista_t *l, bool enrolada = true)
{
    for (int i = 0; i < 32; i++) sv->maestra[i] = (uint8_t)(0x10 + i);
    memcpy(t->uid, UID, 7);
    uint8_t k[16];
    clave_de_tarjeta(sv->maestra, UID, 7, k);
    if (enrolada) memcpy(t->clave, k, 16); else memset(t->clave, 0, 16);   /* de fabrica */
    lista_vaciar(l);
    lista_agregar(l, UID, 7);
}

static auth_resultado_t correr(Servicios *sv, Tarjeta *t, const lista_t *l, const uint8_t *uid = UID, size_t largo = 7)
{
    Ctx c = {sv, t};
    auth_ops_t ops = {apdu_c, derivar_c, azar_c, &c};
    return auth_verificar(l, uid, largo, &ops);
}

int main()
{
    {   /* camino feliz: tarjeta enrolada, en la lista, con su clave */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        VERIFICAR(correr(&sv, &t, &l) == AUTH_OK);
        VERIFICAR(t.apdus == 4);                    /* SELECT, parte 1, parte 2, GetCardUID */
    }
    {   /* fuera de la lista: no se le habla a la tarjeta ni se deriva nada */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        lista_vaciar(&l);
        VERIFICAR(correr(&sv, &t, &l) == AUTH_NO_AUTORIZADA);
        VERIFICAR(t.apdus == 0);
        VERIFICAR(sv.derivaciones == 0);
    }
    {   /* sticker de fabrica (clave en ceros) que esta en la lista: rechazado */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l, false);
        VERIFICAR(correr(&sv, &t, &l) == AUTH_CLAVE_RECHAZADA);
    }
    {   /* tarjeta enrolada con OTRA maestra: rechazada */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        sv.maestra[0] ^= 1;
        VERIFICAR(correr(&sv, &t, &l) == AUTH_CLAVE_RECHAZADA);
    }
    {   /* clon: dice el UID de una tarjeta de la lista pero no tiene su clave */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        for (int i = 0; i < 16; i++) t.clave[i] = (uint8_t)(0x77 + i);
        VERIFICAR(correr(&sv, &t, &l) == AUTH_CLAVE_RECHAZADA);
    }
    {   /* tarjeta que contesta bien la parte 2 pero con un RndA' falso */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        t.mentir_rnda = true;
        VERIFICAR(correr(&sv, &t, &l) == AUTH_ERR_PROTOCOLO);
    }
    {   /* UID real distinto del leido por el aire */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        t.uid_falso_en_canal = true;
        VERIFICAR(correr(&sv, &t, &l) == AUTH_UID_NO_COINCIDE);
    }
    {   /* MAC de la respuesta adulterado */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        t.mac_roto = true;
        VERIFICAR(correr(&sv, &t, &l) == AUTH_ERR_PROTOCOLO);
    }
    {   /* sin la aplicacion de la NTAG 424 DNA (otra tarjeta) */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        t.sin_aplicacion = true;
        VERIFICAR(correr(&sv, &t, &l) == AUTH_ERR_SELECCION);
        VERIFICAR(t.apdus == 1);
    }
    for (int falla = 0; falla < 4; falla++) {   /* la tarjeta deja de contestar en cada paso */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        t.callar_en = falla;
        VERIFICAR(correr(&sv, &t, &l) == AUTH_ERR_COMUNICACION);
    }
    {   /* no se pudo derivar la clave: no se le habla a la tarjeta */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        sv.derivar_ok = false;
        VERIFICAR(correr(&sv, &t, &l) == AUTH_ERR_SERVICIO);
        VERIFICAR(t.apdus == 0);
    }
    {   /* sin azar: tampoco */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        sv.azar_ok = false;
        VERIFICAR(correr(&sv, &t, &l) == AUTH_ERR_SERVICIO);
        VERIFICAR(t.apdus == 0);
    }
    {   /* UID de 4 bytes en la lista: no se autentica */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        const uint8_t u4[4] = {0xAB, 0xE8, 0xB0, 0x22};
        lista_agregar(&l, u4, 4);
        VERIFICAR(correr(&sv, &t, &l, u4, 4) == AUTH_UID_NO_SOPORTADO);
        VERIFICAR(t.apdus == 0);
    }
    {   /* dos tarjetas enroladas tienen claves distintas: la de una no abre con el UID de la otra */
        Servicios sv; Tarjeta t; lista_t l;
        preparar(&sv, &t, &l);
        const uint8_t otro[7] = {0x04, 0x45, 0x66, 0xBA, 0x77, 0x23, 0x90};
        lista_agregar(&l, otro, 7);
        Tarjeta t2;
        memcpy(t2.uid, otro, 7);
        uint8_t k[16];
        clave_de_tarjeta(sv.maestra, UID, 7, k);       /* la clave de la PRIMERA */
        memcpy(t2.clave, k, 16);
        VERIFICAR(correr(&sv, &t2, &l, otro, 7) == AUTH_CLAVE_RECHAZADA);
    }
    {   /* los textos */
        VERIFICAR(strcmp(auth_texto(AUTH_OK), "ok") == 0);
        VERIFICAR(strcmp(auth_texto(AUTH_CLAVE_RECHAZADA), "clave_rechazada") == 0);
        VERIFICAR(strcmp(auth_texto(AUTH_UID_NO_COINCIDE), "uid_no_coincide") == 0);
    }
    RESUMEN();
}
