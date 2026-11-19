#include "aserciones.h"
#include "../ev2_auth.h"

// Lado etiqueta de la mensajeria segura, escrito aparte con las mismas primitivas.
// La coincidencia con NXP ya se verifico contra una tarjeta real (MAC y cifrado de sesion).
static void sesion_de_prueba(ev2_sesion *s) {
  memset(s, 0, sizeof(*s));
  for (int i = 0; i < 16; i++) { s->k_ses_enc[i] = (uint8_t)(0x30 + i); s->k_ses_mac[i] = (uint8_t)(0x50 + i); }
  s->ti[0] = 1; s->ti[1] = 2; s->ti[2] = 3; s->ti[3] = 4;
  s->paso = 3;
}

// Arma la respuesta de la tarjeta: datos || MAC8(rc || ctr+1 || TI || datos) || 91 00
static size_t respuesta_tarjeta(const ev2_sesion *s, const uint8_t *datos, size_t nd, uint8_t *out) {
  memcpy(out, datos, nd);
  uint8_t m[80];
  size_t k = 0;
  uint16_t ctr = (uint16_t)(s->cmd_ctr + 1);
  m[k++] = 0; m[k++] = (uint8_t)(ctr & 0xFF); m[k++] = (uint8_t)(ctr >> 8);
  memcpy(&m[k], s->ti, 4); k += 4;
  memcpy(&m[k], datos, nd); k += nd;
  uint8_t t[16];
  aes_cmac(s->k_ses_mac, m, k, t);
  ev2_truncar(t, &out[nd]);
  out[nd + 8] = 0x91; out[nd + 9] = 0x00;
  return nd + 10;
}

static void test_truncar() {
  uint8_t m[16], o[8];
  for (int i = 0; i < 16; i++) m[i] = (uint8_t)i;
  ev2_truncar(m, o);
  VERIFICAR(o[0] == 1 && o[1] == 3 && o[7] == 15);
}

static void test_apdu_mac_y_respuesta() {
  ev2_sesion s;
  sesion_de_prueba(&s);
  uint8_t ap[48];
  const uint8_t kn = 0;
  size_t n = ev2_apdu_mac(&s, 0x64, &kn, 1, nullptr, 0, ap);
  VERIFICAR(n == 5 + 1 + 8 + 1);
  VERIFICAR(ap[0] == 0x90 && ap[1] == 0x64 && ap[4] == 9 && ap[n - 1] == 0x00);
  uint8_t mac[8];
  ev2_mac_comando(&s, 0x64, &kn, 1, nullptr, 0, mac);
  VERIFICAR(memcmp(&ap[6], mac, 8) == 0);
  // el MAC depende del contador
  s.cmd_ctr = 1;
  uint8_t mac2[8];
  ev2_mac_comando(&s, 0x64, &kn, 1, nullptr, 0, mac2);
  VERIFICAR(memcmp(mac, mac2, 8) != 0);
  s.cmd_ctr = 0;

  uint8_t resp[40], dato = 0x00;
  size_t nr = respuesta_tarjeta(&s, &dato, 1, resp);
  size_t nd = 99;
  VERIFICAR(ev2_resp_verificar(&s, resp, nr, &nd) == EV2_OK);
  VERIFICAR(nd == 1 && s.cmd_ctr == 1);
  // un MAC adulterado se rechaza y NO avanza el contador
  s.cmd_ctr = 0;
  resp[1] ^= 0x01;
  VERIFICAR(ev2_resp_verificar(&s, resp, nr, &nd) == EV2_ERR_MAC);
  VERIFICAR(s.cmd_ctr == 0);
  resp[1] ^= 0x01;
  // repetir una respuesta vieja con el contador avanzado falla (anti-replay)
  VERIFICAR(ev2_resp_verificar(&s, resp, nr, &nd) == EV2_OK);
  VERIFICAR(ev2_resp_verificar(&s, resp, nr, &nd) == EV2_ERR_MAC);
}

static void test_errores_de_respuesta() {
  ev2_sesion s;
  sesion_de_prueba(&s);
  uint8_t resp[40] = {0};
  size_t nd;
  VERIFICAR(ev2_resp_verificar(&s, resp, 9, &nd) == EV2_ERR_LARGO);
  resp[10] = 0x91; resp[11] = 0x1E;
  VERIFICAR(ev2_resp_verificar(&s, resp, 12, &nd) == EV2_ERR_ESTADO);
  s.paso = 2;
  resp[10] = 0x91; resp[11] = 0x00;
  VERIFICAR(ev2_resp_verificar(&s, resp, 12, &nd) == EV2_ERR_SECUENCIA);
}

static void test_cambiar_clave0() {
  ev2_sesion s;
  sesion_de_prueba(&s);
  s.cmd_ctr = 3;
  uint8_t nueva[16];
  for (int i = 0; i < 16; i++) nueva[i] = (uint8_t)(0xC0 + i);
  uint8_t ap[64];
  size_t n = ev2_apdu_cambiar_clave0(&s, nueva, 0x01, ap);
  VERIFICAR(n == 47);
  VERIFICAR(ap[0] == 0x90 && ap[1] == 0xC4 && ap[4] == 41 && ap[5] == 0x00);

  // La tarjeta: verifica el MAC, descifra y recupera clave y version
  uint8_t mac[8];
  ev2_mac_comando(&s, 0xC4, &ap[5], 1, &ap[6], 32, mac);
  VERIFICAR(memcmp(&ap[38], mac, 8) == 0);
  uint8_t iv[16], plano[32];
  ev2_iv(&s, 0xA5, 0x5A, iv);
  aes128_ctx c;
  aes128_iniciar(&c, s.k_ses_enc);
  aes128_cbc_descifrar(&c, iv, &ap[6], plano, 32);
  VERIFICAR(memcmp(plano, nueva, 16) == 0);
  VERIFICAR(plano[16] == 0x01 && plano[17] == 0x80);
  for (int i = 18; i < 32; i++) VERIFICAR(plano[i] == 0);
  // el texto cifrado no contiene la clave nueva en claro
  VERIFICAR(memcmp(&ap[6], nueva, 16) != 0);
  // otro contador -> otro IV -> otro cifrado
  s.cmd_ctr = 4;
  uint8_t ap2[64];
  ev2_apdu_cambiar_clave0(&s, nueva, 0x01, ap2);
  VERIFICAR(memcmp(&ap[6], &ap2[6], 32) != 0);
}

static void test_descifrar_respuesta() {
  ev2_sesion s;
  sesion_de_prueba(&s);
  s.cmd_ctr = 1;
  uint8_t plano[16] = {0x04, 1, 2, 3, 4, 5, 6, 0x80}, cif[16], iv[16], d[16];
  ev2_iv(&s, 0x5A, 0xA5, iv);
  aes128_ctx c;
  aes128_iniciar(&c, s.k_ses_enc);
  aes128_cbc_cifrar(&c, iv, plano, cif, 16);
  ev2_resp_descifrar(&s, cif, d, 16);
  VERIFICAR(memcmp(d, plano, 16) == 0);
}

int main() {
  test_truncar();
  test_apdu_mac_y_respuesta();
  test_errores_de_respuesta();
  test_cambiar_clave0();
  test_descifrar_respuesta();
  RESUMEN();
}
