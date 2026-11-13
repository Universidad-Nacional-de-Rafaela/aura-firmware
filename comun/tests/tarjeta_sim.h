#pragma once
/*
 * Tarjeta NTAG 424 DNA simulada para los tests de host: la parte del protocolo que usan la puerta
 * y la estacion de enrolamiento (SELECT, EV2First, GetCardUID, ChangeKey de la clave 0), con
 * fallas que se pueden inyectar. Esta escrita aparte de ev2_auth.h, pero por la misma mano: que
 * ev2_auth.h coincide con una tarjeta real se comprobo en banco (autenticacion, MAC y cifrado de
 * sesion). ChangeKey solo esta probado contra ESTE simulador.
 */
#include <string.h>
#include "../ntag424.h"

struct Tarjeta {
  uint8_t uid[7];
  uint8_t clave[16];            // la clave 0 que de verdad tiene la tarjeta
  uint8_t rnd_b[16];
  ev2_sesion s;                 // la sesion vista desde la tarjeta
  // contadores
  int apdus = 0;
  int cambios = 0;              // ChangeKey recibidos
  int auth_rechazadas = 0;      // autenticaciones fallidas
  // fallas inyectadas
  int callar_en = -1;           // numero de APDU (0..) al que no contesta, una vez
  bool oscura = false;          // desde ahora no contesta nada
  bool sin_aplicacion = false;
  bool mentir_rnda = false;
  bool uid_falso_en_canal = false;
  bool mac_roto = false;
  bool rechazar_cambio = false;         // ChangeKey -> 91 1E, no cambia nada
  bool ignorar_cambio = false;          // ChangeKey -> 91 00 pero no cambia nada
  bool perder_ack_cambio = false;       // aplica el cambio y la respuesta no llega (una vez)
  bool oscura_tras_cambio = false;      // aplica el cambio y deja de contestar para siempre
  bool aplicar_otra_clave = false;      // aplica una clave distinta de la pedida
};

static inline int tarjeta_apdu(void *ctx, const uint8_t *cmd, size_t n, uint8_t *resp, size_t max) {
  Tarjeta *t = (Tarjeta *)ctx;
  (void)n;
  (void)max;
  int idx = t->apdus++;
  if (t->oscura || idx == t->callar_en) return -1;
  aes128_ctx c;
  const uint8_t iv0[16] = {0};
  if (cmd[0] == 0x00 && cmd[1] == 0xA4) {  // SELECT
    if (t->sin_aplicacion) { resp[0] = 0x6A; resp[1] = 0x82; return 2; }
    resp[0] = 0x90; resp[1] = 0x00;
    return 2;
  }
  if (cmd[0] == 0x90 && cmd[1] == 0x71) {  // EV2First, parte 1
    for (int i = 0; i < 16; i++) t->rnd_b[i] = (uint8_t)(0x40 + 3 * i);
    aes128_iniciar(&c, t->clave);
    aes128_cbc_cifrar(&c, iv0, t->rnd_b, resp, 16);
    resp[16] = 0x91; resp[17] = 0xAF;
    return 18;
  }
  if (cmd[0] == 0x90 && cmd[1] == 0xAF) {  // parte 2
    aes128_iniciar(&c, t->clave);
    uint8_t d[32];
    aes128_cbc_descifrar(&c, iv0, &cmd[5], d, 32);
    uint8_t esp[16];
    ev2_rotar_izq(t->rnd_b, esp);
    if (memcmp(esp, &d[16], 16) != 0) {
      t->auth_rechazadas++;
      memset(&t->s, 0, sizeof(t->s));
      resp[0] = 0x91; resp[1] = 0xAE;
      return 2;
    }
    memset(&t->s, 0, sizeof(t->s));
    memcpy(t->s.rnd_a, d, 16);
    memcpy(t->s.rnd_b, t->rnd_b, 16);
    t->s.ti[0] = 0x11; t->s.ti[1] = 0x22; t->s.ti[2] = 0x33; t->s.ti[3] = 0x44;
    uint8_t sv[32];
    ev2_vector_sesion(&t->s, 0xA5, 0x5A, sv);
    aes_cmac(t->clave, sv, 32, t->s.k_ses_enc);
    ev2_vector_sesion(&t->s, 0x5A, 0xA5, sv);
    aes_cmac(t->clave, sv, 32, t->s.k_ses_mac);
    uint8_t r[32] = {0};
    memcpy(r, t->s.ti, 4);
    ev2_rotar_izq(t->s.rnd_a, &r[4]);
    if (t->mentir_rnda) r[4] ^= 1;
    aes128_cbc_cifrar(&c, iv0, r, resp, 32);
    resp[32] = 0x91; resp[33] = 0x00;
    t->s.paso = 3;
    return 34;
  }
  if (cmd[0] == 0x90 && cmd[1] == 0x51) {  // GetCardUID
    if (t->s.paso != 3) { resp[0] = 0x91; resp[1] = 0xAE; return 2; }
    uint8_t mac[8];
    ev2_mac_comando(&t->s, 0x51, NULL, 0, NULL, 0, mac);
    if (memcmp(&cmd[5], mac, 8) != 0) { resp[0] = 0x91; resp[1] = 0x1E; return 2; }
    t->s.cmd_ctr++;
    uint8_t plano[16] = {0}, iv[16], cif[16];
    memcpy(plano, t->uid, 7);
    if (t->uid_falso_en_canal) plano[6] ^= 0xFF;
    plano[7] = 0x80;
    ev2_iv(&t->s, 0x5A, 0xA5, iv);
    aes128_ctx e;
    aes128_iniciar(&e, t->s.k_ses_enc);
    aes128_cbc_cifrar(&e, iv, plano, cif, 16);
    memcpy(resp, cif, 16);
    uint8_t m[40];
    size_t k = 0;
    m[k++] = 0;
    m[k++] = (uint8_t)(t->s.cmd_ctr & 0xFF);
    m[k++] = (uint8_t)(t->s.cmd_ctr >> 8);
    memcpy(&m[k], t->s.ti, 4); k += 4;
    memcpy(&m[k], cif, 16); k += 16;
    uint8_t full[16];
    aes_cmac(t->s.k_ses_mac, m, k, full);
    ev2_truncar(full, &resp[16]);
    if (t->mac_roto) resp[16] ^= 1;
    resp[24] = 0x91; resp[25] = 0x00;
    return 26;
  }
  if (cmd[0] == 0x90 && cmd[1] == 0xC4) {  // ChangeKey (solo la clave 0)
    t->cambios++;
    if (t->s.paso != 3) { resp[0] = 0x91; resp[1] = 0xAE; return 2; }
    uint8_t mac[8];
    ev2_mac_comando(&t->s, 0xC4, &cmd[5], 1, &cmd[6], 32, mac);
    if (cmd[5] != 0x00 || memcmp(&cmd[38], mac, 8) != 0) { resp[0] = 0x91; resp[1] = 0x1E; return 2; }
    uint8_t iv[16], plano[32];
    ev2_iv(&t->s, 0xA5, 0x5A, iv);
    aes128_ctx e;
    aes128_iniciar(&e, t->s.k_ses_enc);
    aes128_cbc_descifrar(&e, iv, &cmd[6], plano, 32);
    bool relleno_bien = plano[17] == 0x80;
    for (int i = 18; i < 32; i++) if (plano[i] != 0) relleno_bien = false;
    if (!relleno_bien || t->rechazar_cambio) { resp[0] = 0x91; resp[1] = 0x1E; return 2; }
    memset(&t->s, 0, sizeof(t->s));  // cambiar la clave de la sesion la cierra
    if (!t->ignorar_cambio) {
      memcpy(t->clave, plano, 16);
      if (t->aplicar_otra_clave) t->clave[0] ^= 0xFF;
    }
    if (t->oscura_tras_cambio) { t->oscura = true; return -1; }
    if (t->perder_ack_cambio) { t->perder_ack_cambio = false; return -1; }
    resp[0] = 0x91; resp[1] = 0x00;
    return 2;
  }
  return -1;
}
