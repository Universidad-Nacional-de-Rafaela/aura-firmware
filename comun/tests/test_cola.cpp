#include "../cola_persistente.h"
#include "aserciones.h"

// Almacenamiento en RAM que hace de flash. "Reiniciar" es volver a abrir la
// cola sobre el mismo arreglo: lo que no se escribio ahi, se perdio.
struct Ram {
  uint8_t  datos[64 * 1024];
  bool     fallar_escritura;
  uint32_t escrituras;
};

static bool ram_leer(void* ctx, uint32_t off, void* buf, uint32_t n) {
  Ram* r = (Ram*)ctx;
  if (off + n > sizeof(r->datos)) return false;
  memcpy(buf, r->datos + off, n);
  return true;
}

static bool ram_escribir(void* ctx, uint32_t off, const void* buf, uint32_t n) {
  Ram* r = (Ram*)ctx;
  if (r->fallar_escritura || off + n > sizeof(r->datos)) return false;
  memcpy(r->datos + off, buf, n);
  r->escrituras++;
  return true;
}

static AlmacenAura almacen(Ram* r) {
  AlmacenAura a = {ram_leer, ram_escribir, r};
  return a;
}

static MuestraAura muestra(uint8_t n) {
  MuestraAura m;
  memset(&m, 0, sizeof(m));
  memset(m.ingest_id, n, AURA_INGEST_ID_BYTES);
  m.ts = 1790000000u + n;
  m.largo = (uint8_t)snprintf(m.values, sizeof(m.values), "{\"n\":%u}", n);
  return m;
}

static bool meter(ColaPersistente* c, uint8_t n) {
  MuestraAura m = muestra(n);
  return cola_push(c, &m);
}

static bool es(const MuestraAura* m, uint8_t n) {
  MuestraAura e = muestra(n);
  return memcmp(m, &e, sizeof(e)) == 0;
}

int main() {
  static Ram ram;
  memset(&ram, 0xFF, sizeof(ram));  // flash recien borrada: todo en 0xFF
  ram.fallar_escritura = false;

  ColaPersistente c;
  MuestraAura m;

  // Flash virgen: la cabecera no es valida, se formatea y se informa
  VERIFICAR(cola_abrir(&c, almacen(&ram), 5) == true);
  VERIFICAR(c.formateada == true);
  VERIFICAR(cola_cantidad(&c) == 0);
  VERIFICAR(cola_descartadas(&c) == 0);
  VERIFICAR(cola_peek(&c, &m) == false);
  VERIFICAR(cola_pop(&c) == false);

  // FIFO
  for (uint8_t i = 1; i <= 3; i++) VERIFICAR(meter(&c, i) == true);
  VERIFICAR(cola_cantidad(&c) == 3);
  VERIFICAR(cola_peek(&c, &m) && es(&m, 1));
  VERIFICAR(cola_peek(&c, &m) && es(&m, 1));   // peek no consume
  VERIFICAR(cola_pop(&c) == true);
  VERIFICAR(cola_peek(&c, &m) && es(&m, 2));

  // Reinicio: lo que estaba en la cola sigue ahi, en el mismo orden
  ColaPersistente c2;
  VERIFICAR(cola_abrir(&c2, almacen(&ram), 5) == true);
  VERIFICAR(c2.formateada == false);
  VERIFICAR(cola_cantidad(&c2) == 2);
  VERIFICAR(cola_peek(&c2, &m) && es(&m, 2));
  VERIFICAR(cola_pop(&c2));
  VERIFICAR(cola_peek(&c2, &m) && es(&m, 3));
  VERIFICAR(cola_pop(&c2));
  VERIFICAR(cola_cantidad(&c2) == 0);

  // Desborde: con la cola llena se pisa la mas vieja y se cuenta
  for (uint8_t i = 10; i < 17; i++) meter(&c2, i);   // 7 en capacidad 5
  VERIFICAR(cola_cantidad(&c2) == 5);
  VERIFICAR(cola_descartadas(&c2) == 2);
  VERIFICAR(cola_peek(&c2, &m) && es(&m, 12));

  // ...y el desborde tambien sobrevive al reinicio, incluido el contador
  ColaPersistente c3;
  VERIFICAR(cola_abrir(&c3, almacen(&ram), 5));
  VERIFICAR(cola_cantidad(&c3) == 5);
  VERIFICAR(cola_descartadas(&c3) == 2);
  for (uint8_t esperado = 12; esperado < 17; esperado++) {
    VERIFICAR(cola_peek(&c3, &m) && es(&m, esperado));
    VERIFICAR(cola_pop(&c3));
  }
  VERIFICAR(cola_cantidad(&c3) == 0);

  // Vuelta completa del anillo varias veces
  for (int vuelta = 0; vuelta < 4; vuelta++) {
    for (uint8_t i = 0; i < 4; i++) meter(&c3, (uint8_t)(20 + i));
    for (uint8_t i = 0; i < 4; i++) {
      VERIFICAR(cola_peek(&c3, &m) && es(&m, (uint8_t)(20 + i)));
      cola_pop(&c3);
    }
  }
  VERIFICAR(cola_cantidad(&c3) == 0);

  // Otra capacidad que la guardada: no se interpreta con otro tamano, se formatea
  meter(&c3, 30);
  ColaPersistente c4;
  VERIFICAR(cola_abrir(&c4, almacen(&ram), 6));
  VERIFICAR(c4.formateada == true);
  VERIFICAR(cola_cantidad(&c4) == 0);

  // Cabecera corrupta (un bit cambiado): se formatea, no se lee basura
  meter(&c4, 31);
  ram.datos[8] ^= 0x01;
  ColaPersistente c5;
  VERIFICAR(cola_abrir(&c5, almacen(&ram), 6));
  VERIFICAR(c5.formateada == true);
  VERIFICAR(cola_cantidad(&c5) == 0);

  // Si la flash falla al escribir, push devuelve false y el estado no cambia
  meter(&c5, 40);
  ram.fallar_escritura = true;
  VERIFICAR(meter(&c5, 41) == false);
  VERIFICAR(cola_cantidad(&c5) == 1);
  VERIFICAR(cola_pop(&c5) == false);
  VERIFICAR(cola_cantidad(&c5) == 1);
  ram.fallar_escritura = false;
  VERIFICAR(cola_peek(&c5, &m) && es(&m, 40));

  // Capacidad 1: la nueva pisa siempre a la anterior
  ColaPersistente uno;
  static Ram ram1;
  memset(&ram1, 0, sizeof(ram1));
  VERIFICAR(cola_abrir(&uno, almacen(&ram1), 1));
  meter(&uno, 1);
  meter(&uno, 2);
  VERIFICAR(cola_cantidad(&uno) == 1);
  VERIFICAR(cola_descartadas(&uno) == 1);
  VERIFICAR(cola_peek(&uno, &m) && es(&m, 2));

  // Capacidad 0 no tiene sentido
  ColaPersistente cero;
  VERIFICAR(cola_abrir(&cero, almacen(&ram1), 0) == false);

  // Tamano en flash: lo que ocupa la capacidad por defecto
  VERIFICAR(cola_bytes(AURA_COLA_CAP) == sizeof(CabeceraCola) + AURA_COLA_CAP * sizeof(MuestraAura));
  VERIFICAR(cola_bytes(AURA_COLA_CAP) < 300u * 1024u);

  RESUMEN();
}
