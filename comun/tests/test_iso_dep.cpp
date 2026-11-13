#include "aserciones.h"
#include "../iso_dep.h"
#include <vector>

// Tarjeta a nivel de tramas: lo que ve la funcion `trama` (sin CRC).
struct Tarj {
  std::vector<std::vector<uint8_t>> respuestas;   // respuestas encoladas, una por trama recibida
  std::vector<std::vector<uint8_t>> recibidas;    // lo que le mandaron
  size_t i = 0;
};

static int trama_sim(void *ctx, const uint8_t *tx, size_t n, uint8_t *rx, size_t max) {
  Tarj *t = (Tarj *)ctx;
  t->recibidas.emplace_back(tx, tx + n);
  if (t->i >= t->respuestas.size()) return -1;   // la tarjeta deja de contestar
  const auto &r = t->respuestas[t->i++];
  if (r.size() > max) return -1;
  memcpy(rx, r.data(), r.size());
  return (int)r.size();
}

static isodep_t nuevo(Tarj *t) {
  isodep_t d = {trama_sim, t, 0, 64};
  return d;
}

static void test_rats() {
  Tarj t;
  t.respuestas = {{0x06, 0x75, 0x77, 0x81, 0x02, 0x80}};   // ATS de una NTAG 424 DNA: FSCI = 5
  isodep_t d = nuevo(&t);
  d.bloque = 1;
  VERIFICAR(isodep_rats(&d));
  VERIFICAR(d.fsc == 64);
  VERIFICAR(d.bloque == 0);                                // el RATS reinicia la numeracion
  VERIFICAR(t.recibidas[0].size() == 2 && t.recibidas[0][0] == 0xE0 && t.recibidas[0][1] == 0x50);
  // ATS cuyo largo no coincide con el declarado: no es una ATS
  Tarj t2;
  t2.respuestas = {{0x06, 0x75, 0x77}};
  isodep_t d2 = nuevo(&t2);
  VERIFICAR(!isodep_rats(&d2));
  // sin respuesta
  Tarj t3;
  isodep_t d3 = nuevo(&t3);
  VERIFICAR(!isodep_rats(&d3));
  // ATS de un solo byte: se asume la trama chica
  Tarj t4;
  t4.respuestas = {{0x01}};
  isodep_t d4 = nuevo(&t4);
  VERIFICAR(isodep_rats(&d4));
  VERIFICAR(d4.fsc == 32);
}

static void test_apdu_simple() {
  Tarj t;
  t.respuestas = {{0x02, 0x90, 0x00}, {0x03, 0x11, 0x22, 0x91, 0x00}};
  isodep_t d = nuevo(&t);
  const uint8_t cmd[] = {0x00, 0xA4, 0x04};
  uint8_t resp[32];
  int n = isodep_apdu(&d, cmd, sizeof(cmd), resp, sizeof(resp));
  VERIFICAR(n == 2 && resp[0] == 0x90 && resp[1] == 0x00);
  VERIFICAR(t.recibidas[0].size() == 4 && t.recibidas[0][0] == 0x02);   // bloque I numero 0
  VERIFICAR(d.bloque == 1);
  n = isodep_apdu(&d, cmd, sizeof(cmd), resp, sizeof(resp));
  VERIFICAR(n == 4 && resp[0] == 0x11 && resp[3] == 0x00);
  VERIFICAR(t.recibidas[1][0] == 0x03);                                 // ahora el numero 1
  VERIFICAR(d.bloque == 0);
}

static void test_wtx() {
  Tarj t;
  t.respuestas = {{0xF2, 0x05}, {0xF2, 0x05}, {0x02, 0x90, 0x00}};
  isodep_t d = nuevo(&t);
  const uint8_t cmd[] = {0x90, 0x51};
  uint8_t resp[16];
  VERIFICAR(isodep_apdu(&d, cmd, sizeof(cmd), resp, sizeof(resp)) == 2);
  // a cada S(WTX) se le contesta con el mismo S(WTX)
  VERIFICAR(t.recibidas[1].size() == 2 && t.recibidas[1][0] == 0xF2 && t.recibidas[1][1] == 0x05);
  VERIFICAR(t.recibidas[2][0] == 0xF2);
  VERIFICAR(d.bloque == 1);                                             // la numeracion no se movio por el WTX
  // una tarjeta que pide tiempo sin parar no cuelga el lector
  Tarj t2;
  for (int i = 0; i < 20; i++) t2.respuestas.push_back({0xF2, 0x01});
  isodep_t d2 = nuevo(&t2);
  VERIFICAR(isodep_apdu(&d2, cmd, sizeof(cmd), resp, sizeof(resp)) < 0);
  VERIFICAR((int)t2.recibidas.size() <= ISODEP_MAX_WTX + 2);
}

static void test_encadenado() {
  Tarj t;
  // I(0) encadenada, I(1) encadenada, I(0) final
  t.respuestas = {{0x12, 0xAA, 0xBB}, {0x13, 0xCC}, {0x02, 0xDD, 0x90, 0x00}};
  isodep_t d = nuevo(&t);
  const uint8_t cmd[] = {0x90, 0x60};
  uint8_t resp[16];
  int n = isodep_apdu(&d, cmd, sizeof(cmd), resp, sizeof(resp));
  VERIFICAR(n == 6);
  VERIFICAR(resp[4] == 0x90 && resp[5] == 0x00);
  VERIFICAR(resp[0] == 0xAA && resp[1] == 0xBB && resp[2] == 0xCC && resp[3] == 0xDD);
  VERIFICAR(t.recibidas[1].size() == 1 && t.recibidas[1][0] == 0xA3);   // R(ACK) con el bloque que toca (1)
  VERIFICAR(t.recibidas[2][0] == 0xA2);                                 // y despues el 0
  VERIFICAR(d.bloque == 1);                                             // tres bloques recibidos: 0 -> 1 -> 0 -> 1
}

static void test_errores() {
  const uint8_t cmd[] = {0x90, 0x51};
  uint8_t resp[16];
  {   // sin respuesta
    Tarj t;
    isodep_t d = nuevo(&t);
    VERIFICAR(isodep_apdu(&d, cmd, sizeof(cmd), resp, sizeof(resp)) < 0);
  }
  {   // contesta con el bloque equivocado
    Tarj t;
    t.respuestas = {{0x03, 0x90, 0x00}};
    isodep_t d = nuevo(&t);
    VERIFICAR(isodep_apdu(&d, cmd, sizeof(cmd), resp, sizeof(resp)) < 0);
  }
  {   // contesta con un bloque R (NAK) o S que no es WTX
    Tarj t;
    t.respuestas = {{0xB2}};
    isodep_t d = nuevo(&t);
    VERIFICAR(isodep_apdu(&d, cmd, sizeof(cmd), resp, sizeof(resp)) < 0);
    Tarj t2;
    t2.respuestas = {{0xC2}};     // S(DESELECT)
    isodep_t d2 = nuevo(&t2);
    VERIFICAR(isodep_apdu(&d2, cmd, sizeof(cmd), resp, sizeof(resp)) < 0);
  }
  {   // la respuesta no entra en el buffer del que llama
    Tarj t;
    t.respuestas = {{0x02, 1, 2, 3, 4, 5, 6, 7, 8}};
    isodep_t d = nuevo(&t);
    VERIFICAR(isodep_apdu(&d, cmd, sizeof(cmd), resp, 4) < 0);
  }
  {   // un APDU que no entra en una trama (la tarjeta acepta 32)
    Tarj t;
    isodep_t d = nuevo(&t);
    d.fsc = 32;
    uint8_t grande[40] = {0};
    VERIFICAR(isodep_apdu(&d, grande, 30, resp, sizeof(resp)) < 0);
    VERIFICAR(t.recibidas.empty());                                     // ni lo intenta
  }
  {   // la tarjeta se va a mitad de una respuesta encadenada
    Tarj t;
    t.respuestas = {{0x12, 0xAA}};
    isodep_t d = nuevo(&t);
    VERIFICAR(isodep_apdu(&d, cmd, sizeof(cmd), resp, sizeof(resp)) < 0);
  }
}

int main() {
  test_rats();
  test_apdu_simple();
  test_wtx();
  test_encadenado();
  test_errores();
  RESUMEN();
}
