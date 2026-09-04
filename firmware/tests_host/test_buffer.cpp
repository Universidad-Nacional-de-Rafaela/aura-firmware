#include "../comun/buffer_circular.h"
#include "aserciones.h"

static TramaAura hacer(uint16_t seq) {
  const uint8_t A[6] = {0xAA,0,0,0,0,1};
  const uint8_t B[6] = {0xBB,0,0,0,0,2};
  TramaAura t;
  aura_trama_init(&t, AURA_TIPO_TELEMETRIA, A, B, seq, NULL, 0);
  return t;
}

int main() {
  BufferCircular b;
  buffer_init(&b);

  // Arranca vacio
  VERIFICAR(buffer_cantidad(&b) == 0);
  VERIFICAR(buffer_descartados(&b) == 0);
  TramaAura salida;
  VERIFICAR(buffer_pop(&b, &salida) == false);

  // FIFO: sale en el orden que entro
  TramaAura t1 = hacer(1), t2 = hacer(2);
  buffer_push(&b, &t1);
  buffer_push(&b, &t2);
  VERIFICAR(buffer_cantidad(&b) == 2);
  VERIFICAR(buffer_pop(&b, &salida) == true);
  VERIFICAR(salida.seq == 1);
  VERIFICAR(buffer_pop(&b, &salida) == true);
  VERIFICAR(salida.seq == 2);
  VERIFICAR(buffer_cantidad(&b) == 0);

  // peek no consume
  buffer_init(&b);
  TramaAura t42 = hacer(42);
  buffer_push(&b, &t42);
  VERIFICAR(buffer_peek(&b, &salida) == true);
  VERIFICAR(salida.seq == 42);
  VERIFICAR(buffer_cantidad(&b) == 1);

  // Llenar exactamente hasta la capacidad no descarta nada
  buffer_init(&b);
  for (uint16_t i = 0; i < AURA_BUFFER_CAP; i++) { TramaAura x = hacer(i); buffer_push(&b, &x); }
  VERIFICAR(buffer_cantidad(&b) == AURA_BUFFER_CAP);
  VERIFICAR(buffer_descartados(&b) == 0);

  // Pasarse pisa el MAS VIEJO y lo cuenta
  TramaAura t999 = hacer(999);
  buffer_push(&b, &t999);
  VERIFICAR(buffer_cantidad(&b) == AURA_BUFFER_CAP);
  VERIFICAR(buffer_descartados(&b) == 1);
  VERIFICAR(buffer_peek(&b, &salida) == true);
  VERIFICAR(salida.seq == 1);  // el 0 se perdio, ahora el mas viejo es el 1

  // El nuevo quedo al final
  for (uint16_t i = 0; i < AURA_BUFFER_CAP - 1; i++) buffer_pop(&b, &salida);
  VERIFICAR(buffer_pop(&b, &salida) == true);
  VERIFICAR(salida.seq == 999);
  VERIFICAR(buffer_cantidad(&b) == 0);

  // Uso prolongado: el indice circular no se desalinea
  buffer_init(&b);
  for (uint16_t i = 0; i < 500; i++) {
    TramaAura x = hacer(i);
    buffer_push(&b, &x);
    VERIFICAR(buffer_pop(&b, &salida) == true);
    VERIFICAR(salida.seq == i);
  }
  VERIFICAR(buffer_cantidad(&b) == 0);
  VERIFICAR(buffer_descartados(&b) == 0);

  RESUMEN();
}
