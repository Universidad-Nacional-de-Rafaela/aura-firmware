#include "../ingest_id.h"
#include "aserciones.h"
#include <cstring>

// Generador fijo: los tests no dependen del azar.
static uint32_t estado = 1;
static uint32_t azar_fijo() {
  estado = estado * 1664525u + 1013904223u;
  return estado;
}
static uint32_t azar_todo_unos() { return 0xFFFFFFFFu; }
static uint32_t azar_todo_ceros() { return 0; }

static bool es_hex_minuscula(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

int main() {
  uint8_t id[AURA_INGEST_ID_BYTES];
  char a[37], b[37];

  // Formato: 36 caracteres, guiones en 8-13-18-23, hex en minuscula
  aura_ingest_id_nuevo(id, azar_fijo);
  aura_ingest_id_texto(id, a);
  VERIFICAR(strlen(a) == 36);
  VERIFICAR(a[8] == '-' && a[13] == '-' && a[18] == '-' && a[23] == '-');
  for (int i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    VERIFICAR(es_hex_minuscula(a[i]));
  }

  // UUID v4 (RFC 9562): version 4 y variante 10xx, sea cual sea el azar.
  // Sin esto, un backend que valide el UUID podria rechazarlo.
  aura_ingest_id_nuevo(id, azar_todo_unos);
  aura_ingest_id_texto(id, a);
  VERIFICAR(a[14] == '4');
  VERIFICAR(a[19] == 'b');
  VERIFICAR(strcmp(a, "ffffffff-ffff-4fff-bfff-ffffffffffff") == 0);

  aura_ingest_id_nuevo(id, azar_todo_ceros);
  aura_ingest_id_texto(id, a);
  VERIFICAR(strcmp(a, "00000000-0000-4000-8000-000000000000") == 0);

  // El texto es funcion pura de los 16 bytes: el gateway lo arma igual en
  // cada reintento, que es lo que deduplica en ts_telemetry.
  aura_ingest_id_nuevo(id, azar_fijo);
  aura_ingest_id_texto(id, a);
  aura_ingest_id_texto(id, b);
  VERIFICAR(strcmp(a, b) == 0);

  // Dos ids seguidos son distintos
  uint8_t otro[AURA_INGEST_ID_BYTES];
  aura_ingest_id_nuevo(otro, azar_fijo);
  VERIFICAR(!aura_ingest_id_igual(id, otro));
  VERIFICAR(aura_ingest_id_igual(id, id));

  // Mil ids, ninguno repetido con el generador de prueba
  static char vistos[1000][37];
  int repetidos = 0;
  for (int i = 0; i < 1000; i++) {
    aura_ingest_id_nuevo(id, azar_fijo);
    aura_ingest_id_texto(id, vistos[i]);
    for (int j = 0; j < i; j++)
      if (strcmp(vistos[i], vistos[j]) == 0) repetidos++;
  }
  VERIFICAR(repetidos == 0);

  // Bytes conocidos -> texto conocido (orden de bytes = orden del texto)
  const uint8_t fijo[16] = {0x5f,0x0c,0x1b,0x1e,0x8a,0x6d,0x4a,0x55,
                            0x9f,0x2b,0x7c,0x3e,0x2d,0x1a,0x0b,0x99};
  aura_ingest_id_texto(fijo, a);
  VERIFICAR(strcmp(a, "5f0c1b1e-8a6d-4a55-9f2b-7c3e2d1a0b99") == 0);

  RESUMEN();
}
