#include "aserciones.h"
#include "../iso14443a.h"
#include <vector>

// Tarjeta tipo A simulada: UID de 4, 7 o 10 bytes, con el cascade tag y los SAK que corresponden.
struct Tarj {
  std::vector<uint8_t> uid;
  uint8_t sak_final = 0x20;
  bool presente = true;
  bool dormida = false;            // en HALT: solo contesta WUPA
  bool bcc_roto = false;
  bool crc_roto = false;
  bool colision = false;           // contesta con una trama de largo equivocado
  bool sak_mentiroso = false;
  int tramas = 0;
  std::vector<std::vector<uint8_t>> vistas;
};

static std::vector<uint8_t> bytes_nivel(const Tarj *t, int nivel, bool *ultimo) {
  size_t n = t->uid.size();
  std::vector<uint8_t> p;
  if (n == 4) { p.assign(t->uid.begin(), t->uid.end()); *ultimo = true; }
  else if (n == 7) {
    if (nivel == 0) { p = {0x88, t->uid[0], t->uid[1], t->uid[2]}; *ultimo = false; }
    else { p.assign(t->uid.begin() + 3, t->uid.end()); *ultimo = true; }
  } else {
    if (nivel == 0) { p = {0x88, t->uid[0], t->uid[1], t->uid[2]}; *ultimo = false; }
    else if (nivel == 1) { p = {0x88, t->uid[3], t->uid[4], t->uid[5]}; *ultimo = false; }
    else { p.assign(t->uid.begin() + 6, t->uid.end()); *ultimo = true; }
  }
  return p;
}

static int sim(void *ctx, const uint8_t *tx, size_t ntx, uint8_t bits, uint8_t *rx, size_t max) {
  Tarj *t = (Tarj *)ctx;
  (void)max;
  t->tramas++;
  t->vistas.emplace_back(tx, tx + ntx);
  if (!t->presente) return -1;
  if (ntx == 1 && bits == 7 && (tx[0] == 0x26 || tx[0] == 0x52)) {      // REQA / WUPA
    if (t->dormida && tx[0] == 0x26) return -1;
    t->dormida = false;
    rx[0] = 0x44; rx[1] = 0x00;
    return 2;
  }
  if (ntx == 4 && tx[0] == 0x50 && tx[1] == 0x00) {                      // HLTA
    t->dormida = true;
    return -1;
  }
  int nivel = tx[0] == 0x93 ? 0 : tx[0] == 0x95 ? 1 : tx[0] == 0x97 ? 2 : -1;
  if (nivel < 0) return -1;
  bool ultimo;
  std::vector<uint8_t> p = bytes_nivel(t, nivel, &ultimo);
  if (ntx == 2 && tx[1] == 0x20) {                                       // anticolision
    if (t->colision) { rx[0] = 1; rx[1] = 2; return 2; }
    memcpy(rx, p.data(), 4);
    rx[4] = (uint8_t)(p[0] ^ p[1] ^ p[2] ^ p[3]);
    if (t->bcc_roto) rx[4] ^= 1;
    return 5;
  }
  if (ntx == 9 && tx[1] == 0x70) {                                       // select
    rx[0] = ultimo ? t->sak_final : 0x04;
    if (t->sak_mentiroso) rx[0] ^= 0x04;
    uint8_t c[2];
    iso14443a_crc(rx, 1, c);
    rx[1] = c[0]; rx[2] = c[1];
    if (t->crc_roto) rx[2] ^= 1;
    return 3;
  }
  return -1;
}

static iso14443a_ops_t ops(Tarj *t) { return {sim, t}; }

static void test_crc() {
  uint8_t c[2];
  const uint8_t a[] = {0x12, 0x34};
  iso14443a_crc(a, 2, c);
  VERIFICAR(c[0] == 0x26 && c[1] == 0xCF);          // ISO 14443-3, anexo del CRC_A
  const uint8_t b[] = {0x00, 0x00};
  iso14443a_crc(b, 2, c);
  VERIFICAR(c[0] == 0xA0 && c[1] == 0x1E);
  const uint8_t h[] = {0x50, 0x00};
  iso14443a_crc(h, 2, c);
  VERIFICAR(c[0] == 0x57 && c[1] == 0xCD);          // la trama HLTA conocida: 50 00 57 CD
  const uint8_t ok[] = {0x50, 0x00, 0x57, 0xCD};
  VERIFICAR(iso14443a_crc_ok(ok, 4));
  const uint8_t mal[] = {0x50, 0x00, 0x57, 0xCE};
  VERIFICAR(!iso14443a_crc_ok(mal, 4));
  VERIFICAR(!iso14443a_crc_ok(ok, 2));
}

static void test_uid(std::vector<uint8_t> uid, int largo) {
  Tarj t; t.uid = uid;
  iso14443a_ops_t o = ops(&t);
  uint8_t atqa[2];
  VERIFICAR(iso14443a_despertar(&o, false, atqa) == ISO14443A_OK);
  VERIFICAR(atqa[0] == 0x44);
  iso14443a_tarjeta_t r;
  VERIFICAR(iso14443a_seleccionar(&o, &r) == ISO14443A_OK);
  VERIFICAR(r.largo == largo);
  VERIFICAR(memcmp(r.uid, uid.data(), uid.size()) == 0);
  VERIFICAR(r.sak == 0x20);
  VERIFICAR(t.vistas[1][0] == 0x93 && t.vistas[1][1] == 0x20);
}

static void test_errores() {
  uint8_t atqa[2];
  iso14443a_tarjeta_t r;
  {   // nadie en el campo
    Tarj t; t.uid = {1, 2, 3, 4}; t.presente = false;
    iso14443a_ops_t o = ops(&t);
    VERIFICAR(iso14443a_despertar(&o, false, atqa) == ISO14443A_SIN_TARJETA);
  }
  {   // BCC roto
    Tarj t; t.uid = {1, 2, 3, 4}; t.bcc_roto = true;
    iso14443a_ops_t o = ops(&t);
    VERIFICAR(iso14443a_seleccionar(&o, &r) == ISO14443A_ERR_BCC);
  }
  {   // CRC del SAK roto
    Tarj t; t.uid = {1, 2, 3, 4}; t.crc_roto = true;
    iso14443a_ops_t o = ops(&t);
    VERIFICAR(iso14443a_seleccionar(&o, &r) == ISO14443A_ERR_CRC);
  }
  {   // dos tarjetas: respuesta de largo equivocado
    Tarj t; t.uid = {1, 2, 3, 4}; t.colision = true;
    iso14443a_ops_t o = ops(&t);
    VERIFICAR(iso14443a_seleccionar(&o, &r) == ISO14443A_ERR_ANTICOLISION);
  }
  {   // el SAK no dice lo mismo que el cascade tag
    Tarj t; t.uid = {1, 2, 3, 4, 5, 6, 7}; t.sak_mentiroso = true;
    iso14443a_ops_t o = ops(&t);
    VERIFICAR(iso14443a_seleccionar(&o, &r) == ISO14443A_ERR_SELECCION);
  }
}

static void test_halt_y_wupa() {
  Tarj t; t.uid = {1, 2, 3, 4};
  iso14443a_ops_t o = ops(&t);
  uint8_t atqa[2];
  iso14443a_halt(&o);
  const auto &h = t.vistas.back();
  VERIFICAR(h.size() == 4 && h[0] == 0x50 && h[1] == 0x00 && h[2] == 0x57 && h[3] == 0xCD);
  VERIFICAR(t.dormida);
  VERIFICAR(iso14443a_despertar(&o, false, atqa) == ISO14443A_SIN_TARJETA);   // en HALT el REQA no la despierta
  VERIFICAR(iso14443a_despertar(&o, true, atqa) == ISO14443A_OK);             // el WUPA si
}

int main() {
  test_crc();
  test_uid({0xAB, 0xE8, 0xB0, 0x22}, 4);
  test_uid({0x04, 0x45, 0x66, 0xBA, 0x77, 0x23, 0x90}, 7);
  test_uid({0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99}, 10);
  test_errores();
  test_halt_y_wupa();
  RESUMEN();
}
