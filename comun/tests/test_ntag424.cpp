#include "aserciones.h"
#include "tarjeta_sim.h"

static bool azar_ok(void *, uint8_t out[16]) {
  for (int i = 0; i < 16; i++) out[i] = (uint8_t)(0xA0 + 5 * i);
  return true;
}
static bool azar_falla(void *, uint8_t *) { return false; }

static const uint8_t UID[7] = {0x04, 0x45, 0x65, 0xBA, 0x77, 0x23, 0x90};

static void nueva(Tarjeta *t, uint8_t base) {
  memcpy(t->uid, UID, 7);
  for (int i = 0; i < 16; i++) t->clave[i] = (uint8_t)(base + i);
}

static ntag_ops_t ops_de(Tarjeta *t, bool (*azar)(void *, uint8_t *) = azar_ok) {
  ntag_ops_t o = {tarjeta_apdu, nullptr, azar, t};
  return o;
}

static void test_autenticar() {
  Tarjeta t; nueva(&t, 0x20);
  ntag_ops_t o = ops_de(&t);
  ev2_sesion s;
  VERIFICAR(ntag_autenticar(&o, t.clave, &s) == NTAG_OK);
  VERIFICAR(s.paso == 3);
  VERIFICAR(t.apdus == 3);                           // SELECT, parte 1, parte 2
  uint8_t cero[16] = {0};
  VERIFICAR(memcmp(s.k_ses_enc, cero, 16) != 0);
  uint8_t uid[7];
  VERIFICAR(ntag_leer_uid(&o, &s, uid) == NTAG_OK);
  VERIFICAR(memcmp(uid, UID, 7) == 0);
  VERIFICAR(ntag_confirmar_uid(&o, &s, UID) == NTAG_OK);   // segundo comando de la misma sesion
}

static void test_clave_rechazada() {
  Tarjeta t; nueva(&t, 0x20);
  ntag_ops_t o = ops_de(&t);
  ev2_sesion s;
  uint8_t otra[16] = {0};
  VERIFICAR(ntag_autenticar(&o, otra, &s) == NTAG_CLAVE_RECHAZADA);
  VERIFICAR(t.auth_rechazadas == 1);
  uint8_t cero[16] = {0};
  VERIFICAR(memcmp(&s, cero, 16) == 0 && s.paso == 0);     // no queda nada de la sesion
}

static void test_fallas_de_la_tarjeta() {
  {
    Tarjeta t; nueva(&t, 0x20);
    ntag_ops_t o = ops_de(&t, azar_falla);
    ev2_sesion s;
    VERIFICAR(ntag_autenticar(&o, t.clave, &s) == NTAG_ERR_AZAR);
    VERIFICAR(t.apdus == 0);
  }
  {
    Tarjeta t; nueva(&t, 0x20);
    t.sin_aplicacion = true;
    ntag_ops_t o = ops_de(&t);
    ev2_sesion s;
    VERIFICAR(ntag_autenticar(&o, t.clave, &s) == NTAG_ERR_SELECCION);
  }
  {
    Tarjeta t; nueva(&t, 0x20);
    t.mentir_rnda = true;
    ntag_ops_t o = ops_de(&t);
    ev2_sesion s;
    VERIFICAR(ntag_autenticar(&o, t.clave, &s) == NTAG_ERR_PROTOCOLO);
  }
  for (int falla = 0; falla < 3; falla++) {
    Tarjeta t; nueva(&t, 0x20);
    t.callar_en = falla;
    ntag_ops_t o = ops_de(&t);
    ev2_sesion s;
    VERIFICAR(ntag_autenticar(&o, t.clave, &s) == NTAG_ERR_COMUNICACION);
  }
}

static void test_uid() {
  {
    Tarjeta t; nueva(&t, 0x20);
    t.uid_falso_en_canal = true;
    ntag_ops_t o = ops_de(&t);
    ev2_sesion s;
    VERIFICAR(ntag_autenticar(&o, t.clave, &s) == NTAG_OK);
    VERIFICAR(ntag_confirmar_uid(&o, &s, UID) == NTAG_UID_NO_COINCIDE);
  }
  {
    Tarjeta t; nueva(&t, 0x20);
    t.mac_roto = true;
    ntag_ops_t o = ops_de(&t);
    ev2_sesion s;
    VERIFICAR(ntag_autenticar(&o, t.clave, &s) == NTAG_OK);
    uint8_t uid[7];
    VERIFICAR(ntag_leer_uid(&o, &s, uid) == NTAG_ERR_PROTOCOLO);
  }
}

static void test_cambiar_clave() {
  uint8_t nuevaclave[16];
  for (int i = 0; i < 16; i++) nuevaclave[i] = (uint8_t)(0xC0 + i);
  {   // camino feliz: queda la clave nueva y la vieja deja de servir
    Tarjeta t; nueva(&t, 0x20);
    uint8_t vieja[16];
    memcpy(vieja, t.clave, 16);
    ntag_ops_t o = ops_de(&t);
    ev2_sesion s;
    VERIFICAR(ntag_autenticar(&o, t.clave, &s) == NTAG_OK);
    VERIFICAR(ntag_cambiar_clave0(&o, &s, nuevaclave, 1) == NTAG_OK);
    VERIFICAR(t.cambios == 1);
    VERIFICAR(memcmp(t.clave, nuevaclave, 16) == 0);
    VERIFICAR(s.paso == 0);                                 // la sesion quedo inutilizable
    ev2_sesion s2;
    VERIFICAR(ntag_autenticar(&o, vieja, &s2) == NTAG_CLAVE_RECHAZADA);
    VERIFICAR(ntag_autenticar(&o, nuevaclave, &s2) == NTAG_OK);
  }
  {   // la tarjeta lo rechaza: no cambia nada
    Tarjeta t; nueva(&t, 0x20);
    uint8_t vieja[16];
    memcpy(vieja, t.clave, 16);
    t.rechazar_cambio = true;
    ntag_ops_t o = ops_de(&t);
    ev2_sesion s;
    ntag_autenticar(&o, t.clave, &s);
    VERIFICAR(ntag_cambiar_clave0(&o, &s, nuevaclave, 1) == NTAG_ERR_PROTOCOLO);
    VERIFICAR(memcmp(t.clave, vieja, 16) == 0);
  }
  {   // contesta que si y no cambia: por eso hay que verificar con la clave nueva
    Tarjeta t; nueva(&t, 0x20);
    uint8_t vieja[16];
    memcpy(vieja, t.clave, 16);
    t.ignorar_cambio = true;
    ntag_ops_t o = ops_de(&t);
    ev2_sesion s;
    ntag_autenticar(&o, t.clave, &s);
    VERIFICAR(ntag_cambiar_clave0(&o, &s, nuevaclave, 1) == NTAG_OK);
    VERIFICAR(memcmp(t.clave, vieja, 16) == 0);
  }
  {   // aplica y la respuesta no llega
    Tarjeta t; nueva(&t, 0x20);
    t.perder_ack_cambio = true;
    ntag_ops_t o = ops_de(&t);
    ev2_sesion s;
    ntag_autenticar(&o, t.clave, &s);
    VERIFICAR(ntag_cambiar_clave0(&o, &s, nuevaclave, 1) == NTAG_ERR_COMUNICACION);
    VERIFICAR(memcmp(t.clave, nuevaclave, 16) == 0);
  }
}

int main() {
  test_autenticar();
  test_clave_rechazada();
  test_fallas_de_la_tarjeta();
  test_uid();
  test_cambiar_clave();
  RESUMEN();
}
