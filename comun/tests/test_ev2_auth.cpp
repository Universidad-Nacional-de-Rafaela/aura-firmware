#include "aserciones.h"
#include "../ev2_auth.h"

// Etiqueta simulada: hace su lado del protocolo con las mismas primitivas.
// OJO: esto prueba coherencia interna, NO que la derivacion coincida con NXP;
// eso requiere los vectores de AN12196 o una tarjeta real.
struct EtiquetaSim {
  uint8_t clave[16];
  uint8_t rnd_b[16];
  uint8_t ti[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  uint8_t rnd_a[16];

  void parte1(uint8_t out[18]) {
    aes128_ctx c;
    aes128_iniciar(&c, clave);
    const uint8_t iv[16] = {0};
    aes128_cbc_cifrar(&c, iv, rnd_b, out, 16);
    out[16] = 0x91;
    out[17] = 0xAF;
  }
  // Devuelve false si RndB' no coincide (clave distinta).
  bool parte2(const uint8_t *apdu, uint8_t out[34], bool mentir_rnda = false) {
    aes128_ctx c;
    aes128_iniciar(&c, clave);
    const uint8_t iv[16] = {0};
    uint8_t d[32];
    aes128_cbc_descifrar(&c, iv, &apdu[5], d, 32);
    memcpy(rnd_a, d, 16);
    uint8_t esp[16];
    ev2_rotar_izq(rnd_b, esp);
    if (memcmp(esp, &d[16], 16) != 0) return false;
    uint8_t r[32] = {0};
    memcpy(r, ti, 4);
    ev2_rotar_izq(rnd_a, &r[4]);
    if (mentir_rnda) r[4] ^= 1;
    aes128_cbc_cifrar(&c, iv, r, out, 32);
    out[32] = 0x91;
    out[33] = 0x00;
    return true;
  }
};

static void llenar(uint8_t *p, size_t n, uint8_t base) {
  for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(base + i * 7);
}

static void test_exito() {
  uint8_t clave[16] = {0};
  EtiquetaSim t;
  memcpy(t.clave, clave, 16);
  llenar(t.rnd_b, 16, 0x10);
  uint8_t rnd_a[16];
  llenar(rnd_a, 16, 0xA0);

  ev2_sesion s;
  uint8_t a1[8], a2[38], r1[18], r2[34];
  VERIFICAR(ev2_parte1_apdu(&s, clave, rnd_a, 0, a1) == 8);
  const uint8_t esp1[8] = {0x90, 0x71, 0, 0, 2, 0, 0, 0};
  VERIFICAR(memcmp(a1, esp1, 8) == 0);

  t.parte1(r1);
  size_t largo = 0;
  VERIFICAR(ev2_parte2_apdu(&s, r1, 18, a2, &largo) == EV2_OK);
  VERIFICAR(largo == 38);
  VERIFICAR(a2[0] == 0x90 && a2[1] == 0xAF && a2[4] == 0x20 && a2[37] == 0x00);
  VERIFICAR(memcmp(s.rnd_b, t.rnd_b, 16) == 0);  // descifro bien RndB

  VERIFICAR(t.parte2(a2, r2));
  VERIFICAR(ev2_parte2_resp(&s, r2, 34) == EV2_OK);
  VERIFICAR(memcmp(s.ti, t.ti, 4) == 0);
  VERIFICAR(s.paso == 3);
  // las dos claves de sesion son distintas entre si y de la clave base
  VERIFICAR(memcmp(s.k_ses_enc, s.k_ses_mac, 16) != 0);
  VERIFICAR(memcmp(s.k_ses_enc, clave, 16) != 0);
}

static void test_clave_equivocada() {
  uint8_t clave_lector[16] = {0};
  EtiquetaSim t;
  memset(t.clave, 0x11, 16);  // la tarjeta tiene otra clave
  llenar(t.rnd_b, 16, 0x10);
  uint8_t rnd_a[16];
  llenar(rnd_a, 16, 0xA0);

  ev2_sesion s;
  uint8_t a1[8], a2[38], r1[18];
  ev2_parte1_apdu(&s, clave_lector, rnd_a, 0, a1);
  t.parte1(r1);
  size_t largo;
  VERIFICAR(ev2_parte2_apdu(&s, r1, 18, a2, &largo) == EV2_OK);
  uint8_t r2[34];
  VERIFICAR(!t.parte2(a2, r2));  // la tarjeta no acepta: descifra basura
}

static void test_tarjeta_que_miente() {
  uint8_t clave[16] = {0};
  EtiquetaSim t;
  memcpy(t.clave, clave, 16);
  llenar(t.rnd_b, 16, 0x10);
  uint8_t rnd_a[16];
  llenar(rnd_a, 16, 0xA0);
  ev2_sesion s;
  uint8_t a1[8], a2[38], r1[18], r2[34];
  ev2_parte1_apdu(&s, clave, rnd_a, 0, a1);
  t.parte1(r1);
  size_t largo;
  ev2_parte2_apdu(&s, r1, 18, a2, &largo);
  VERIFICAR(t.parte2(a2, r2, true));
  VERIFICAR(ev2_parte2_resp(&s, r2, 34) == EV2_ERR_RNDA);
  VERIFICAR(s.paso == 2);  // no queda autenticada
}

static void test_errores_de_protocolo() {
  uint8_t clave[16] = {0}, rnd_a[16] = {1};
  ev2_sesion s;
  uint8_t a1[8], a2[38], r[34] = {0};
  size_t largo;
  ev2_parte1_apdu(&s, clave, rnd_a, 0, a1);
  VERIFICAR(ev2_parte2_resp(&s, r, 34) == EV2_ERR_SECUENCIA);  // falta la parte 2 de ida
  VERIFICAR(ev2_parte2_apdu(&s, r, 17, a2, &largo) == EV2_ERR_LARGO);
  r[16] = 0x91;
  r[17] = 0xAE;  // authentication error
  VERIFICAR(ev2_parte2_apdu(&s, r, 18, a2, &largo) == EV2_ERR_ESTADO);
  r[17] = 0xAF;
  VERIFICAR(ev2_parte2_apdu(&s, r, 18, a2, &largo) == EV2_OK);
  r[32] = 0x91;
  r[33] = 0x1E;  // integrity error
  VERIFICAR(ev2_parte2_resp(&s, r, 34) == EV2_ERR_ESTADO);
  VERIFICAR(ev2_parte2_resp(&s, r, 33) == EV2_ERR_LARGO);
}

static void test_rotacion() {
  uint8_t a[16], b[16];
  for (int i = 0; i < 16; i++) a[i] = (uint8_t)i;
  ev2_rotar_izq(a, b);
  VERIFICAR(b[0] == 1 && b[14] == 15 && b[15] == 0);
}

static void test_vector_de_sesion() {
  ev2_sesion s;
  memset(&s, 0, sizeof(s));
  for (int i = 0; i < 16; i++) {
    s.rnd_a[i] = (uint8_t)(0x10 + i);
    s.rnd_b[i] = (uint8_t)(0x80 + i);
  }
  uint8_t sv[32];
  ev2_vector_sesion(&s, 0xA5, 0x5A, sv);
  const uint8_t cab[6] = {0xA5, 0x5A, 0x00, 0x01, 0x00, 0x80};
  VERIFICAR(memcmp(sv, cab, 6) == 0);
  VERIFICAR(sv[6] == 0x10 && sv[7] == 0x11);
  VERIFICAR(sv[8] == (0x12 ^ 0x80) && sv[13] == (0x17 ^ 0x85));
  VERIFICAR(sv[14] == 0x86 && sv[23] == 0x8F);
  VERIFICAR(sv[24] == 0x18 && sv[31] == 0x1F);
}

int main() {
  test_rotacion();
  test_vector_de_sesion();
  test_exito();
  test_clave_equivocada();
  test_tarjeta_que_miente();
  test_errores_de_protocolo();
  RESUMEN();
}
