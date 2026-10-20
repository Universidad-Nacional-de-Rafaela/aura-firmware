#include "../cola_archivos.h"
#include "aserciones.h"

#define RANURA ((uint32_t)sizeof(MuestraAura))
#define CABECERA ((uint32_t)sizeof(CabeceraCola))

// Archivos en RAM que hacen de LittleFS. Cuenta cuantos archivos toca cada
// operacion de la cola: la idea del formato es que cada escritura toque uno solo
// y chico, porque en LittleFS cambiar un byte reescribe el archivo desde ahi.
#define MAX_ARCHIVOS 8
struct Disco {
  uint8_t  datos[MAX_ARCHIVOS][AURA_COLA_RANURAS_POR_ARCHIVO * sizeof(MuestraAura)];
  uint32_t escrituras;
  uint32_t ultimo_archivo;
  uint32_t mayor_escritura;
};

static bool disco_leer(void* ctx, uint32_t archivo, uint32_t off, void* buf, uint32_t n) {
  Disco* d = (Disco*)ctx;
  if (archivo >= MAX_ARCHIVOS || off + n > sizeof(d->datos[0])) return false;
  memcpy(buf, d->datos[archivo] + off, n);
  return true;
}

static bool disco_escribir(void* ctx, uint32_t archivo, uint32_t off, const void* buf, uint32_t n) {
  Disco* d = (Disco*)ctx;
  if (archivo >= MAX_ARCHIVOS || off + n > sizeof(d->datos[0])) return false;
  memcpy(d->datos[archivo] + off, buf, n);
  d->escrituras++;
  d->ultimo_archivo = archivo;
  if (n > d->mayor_escritura) d->mayor_escritura = n;
  return true;
}

static MuestraAura muestra(uint8_t n) {
  MuestraAura m;
  memset(&m, 0, sizeof(m));
  memset(m.ingest_id, n, AURA_INGEST_ID_BYTES);
  m.ts = 1790000000u + n;
  m.largo = (uint8_t)snprintf(m.values, sizeof(m.values), "{\"n\":%u}", n);
  return m;
}

int main() {
  uint32_t archivo, off;
  const uint32_t R = AURA_COLA_RANURAS_POR_ARCHIVO;

  // La cabecera va sola en el archivo 0
  VERIFICAR(cola_tramo(0, CABECERA, &archivo, &off) == CABECERA);
  VERIFICAR(archivo == 0 && off == 0);

  // Ranura 0: primera del archivo 1
  VERIFICAR(cola_tramo(cola_offset_ranura(0), RANURA, &archivo, &off) == RANURA);
  VERIFICAR(archivo == 1 && off == 0);

  // Ultima ranura del archivo 1 y primera del 2
  VERIFICAR(cola_tramo(cola_offset_ranura(R - 1), RANURA, &archivo, &off) == RANURA);
  VERIFICAR(archivo == 1 && off == (R - 1) * RANURA);
  VERIFICAR(cola_tramo(cola_offset_ranura(R), RANURA, &archivo, &off) == RANURA);
  VERIFICAR(archivo == 2 && off == 0);

  // Un rango que cruza de la cabecera a la ranura 0 se corta en el limite
  VERIFICAR(cola_tramo(CABECERA - 4, 10, &archivo, &off) == 4);
  VERIFICAR(archivo == 0 && off == CABECERA - 4);

  // Uno que cruza de un archivo de ranuras al siguiente, tambien
  VERIFICAR(cola_tramo(cola_offset_ranura(R) - 3, 10, &archivo, &off) == 3);
  VERIFICAR(archivo == 1 && off == R * RANURA - 3);

  // Cuantos archivos y de que tamano: el ultimo puede quedar incompleto
  VERIFICAR(cola_archivos_cantidad(1440) == 1 + (1440 + R - 1) / R);
  VERIFICAR(cola_archivos_cantidad(R) == 2);
  VERIFICAR(cola_archivos_cantidad(R + 1) == 3);
  VERIFICAR(cola_archivo_bytes(0, R + 4) == CABECERA);
  VERIFICAR(cola_archivo_bytes(1, R + 4) == R * RANURA);
  VERIFICAR(cola_archivo_bytes(2, R + 4) == 4 * RANURA);
  VERIFICAR(cola_archivo_bytes(3, R + 4) == 0);

  // Leer y escribir un rango que cruza archivos lo reparte bien
  static Disco d;
  memset(&d, 0, sizeof(d));
  ArchivosAura arch = {disco_leer, disco_escribir, &d};
  uint8_t ida[40], vuelta[40];
  for (int i = 0; i < 40; i++) ida[i] = (uint8_t)(i + 1);
  VERIFICAR(cola_archivos_escribir(&arch, CABECERA - 20, ida, sizeof(ida)));
  VERIFICAR(d.escrituras == 2);
  VERIFICAR(d.datos[0][CABECERA - 1] == 20 && d.datos[1][0] == 21);
  VERIFICAR(cola_archivos_leer(&arch, CABECERA - 20, vuelta, sizeof(vuelta)));
  VERIFICAR(memcmp(ida, vuelta, sizeof(ida)) == 0);

  // La cola entera sobre estos archivos: cada escritura toca un solo archivo,
  // y nunca mas de una ranura
  memset(&d, 0xFF, sizeof(d));
  d.escrituras = 0;
  d.mayor_escritura = 0;
  const uint32_t cap = 3 * R;   // tres archivos de ranuras
  AlmacenAura alm = cola_archivos_almacen(&arch);
  ColaPersistente c;
  MuestraAura m;
  VERIFICAR(cola_abrir(&c, alm, cap));
  VERIFICAR(c.formateada);
  VERIFICAR(d.escrituras == 1 && d.ultimo_archivo == 0);

  for (uint32_t i = 0; i < cap + 5; i++) {   // da la vuelta al anillo
    m = muestra((uint8_t)i);
    uint32_t antes = d.escrituras;
    VERIFICAR(cola_push(&c, &m));
    VERIFICAR(d.escrituras == antes + 2);    // ranura y cabecera, una vez cada una
    VERIFICAR(d.ultimo_archivo == 0);        // la cabecera, ultima
  }
  VERIFICAR(d.mayor_escritura == RANURA);
  VERIFICAR(cola_cantidad(&c) == cap);
  VERIFICAR(cola_descartadas(&c) == 5);

  uint32_t antes = d.escrituras;
  MuestraAura esperada = muestra(5);   // las 5 primeras se pisaron
  VERIFICAR(cola_peek(&c, &m) && memcmp(&m, &esperada, sizeof(m)) == 0);
  VERIFICAR(cola_pop(&c));
  VERIFICAR(d.escrituras == antes + 1 && d.ultimo_archivo == 0);

  // "Reinicio": abrir de nuevo sobre los mismos archivos recupera la cola
  ColaPersistente c2;
  VERIFICAR(cola_abrir(&c2, alm, cap));
  VERIFICAR(!c2.formateada);
  VERIFICAR(cola_cantidad(&c2) == cap - 1);
  esperada = muestra(6);
  VERIFICAR(cola_peek(&c2, &m) && memcmp(&m, &esperada, sizeof(m)) == 0);

  RESUMEN();
}
