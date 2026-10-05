#include "../ingest_id.h"
#include "aserciones.h"
#include <cstring>

int main() {
  const uint8_t mac1[6] = {0x24,0x6F,0x28,0x11,0x22,0x33};
  const uint8_t mac2[6] = {0x24,0x6F,0x28,0x11,0x22,0x34};
  char a[37], b[37];

  // Formato: 36 caracteres, guiones en 8-13-18-23
  aura_ingest_id(mac1, 1000, 5, a);
  VERIFICAR(strlen(a) == 36);
  VERIFICAR(a[8] == '-' && a[13] == '-' && a[18] == '-' && a[23] == '-');
  for (int i = 0; i < 36; i++) {
    bool ok = (a[i] == '-') || (a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f');
    VERIFICAR(ok);
  }

  // DETERMINISTICO: la misma entrada da siempre el mismo id.
  // Sin esto, reintentar duplica filas en ts_telemetry.
  aura_ingest_id(mac1, 1000, 5, b);
  VERIFICAR(strcmp(a, b) == 0);

  // Cambiar cualquiera de los tres componentes cambia el id
  aura_ingest_id(mac1, 1000, 6, b);
  VERIFICAR(strcmp(a, b) != 0);
  aura_ingest_id(mac1, 1001, 5, b);
  VERIFICAR(strcmp(a, b) != 0);
  aura_ingest_id(mac2, 1000, 5, b);
  VERIFICAR(strcmp(a, b) != 0);

  // Dos nodos distintos con la misma seq no colisionan
  aura_ingest_id(mac1, 7, 1, a);
  aura_ingest_id(mac2, 7, 1, b);
  VERIFICAR(strcmp(a, b) != 0);

  RESUMEN();
}
