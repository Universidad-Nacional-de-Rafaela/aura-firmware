#include "../main/latido_logica.h"
#include "../../../../comun/tests/aserciones.h"

static const uint8_t A[4] = {0xAB, 0xE8, 0xB0, 0x22};

// Lo que hace main.c al ejecutar las acciones: cada ACC_REPORTAR manda un reporte con el
// intervalo_s vigente en ese momento. Devuelve el del ultimo reporte (0 si no hubo).
static int reportado(puerta_t &p, latido_t &l, int64_t ahora) {
  int intervalo = 0;
  accion_t a;
  while (puerta_sacar(&p, &a))
    if (a.tipo == ACC_REPORTAR) intervalo = latido_al_reportar(&l, &p, ahora);
  return intervalo;
}

int main() {
  // Armada (latido 30 s) y se desarma con el llavero: el reporte ya dice 120 s, no 30
  {
    puerta_t p;
    puerta_iniciar(&p, 0, true, false, PULSO_DEFECTO_MS, AVISO_DEFECTO_S);
    latido_t l;
    latido_iniciar(&l, puerta_latido_s(&p));
    VERIFICAR(l.intervalo_s == LATIDO_ARMADA_S);
    puerta_tarjeta(&p, 1000, A, 4, true);
    VERIFICAR(reportado(p, l, 1000) == LATIDO_DESARMADA_S);
    VERIFICAR(l.intervalo_s == LATIDO_DESARMADA_S);
  }

  // Desarmada (120 s) y se arma con la doble pasada: el reporte ya dice 30 s
  {
    puerta_t p;
    puerta_iniciar(&p, 0, false, false, PULSO_DEFECTO_MS, AVISO_DEFECTO_S);
    latido_t l;
    latido_iniciar(&l, puerta_latido_s(&p));
    puerta_tarjeta(&p, 1000, A, 4, true);
    reportado(p, l, 1000);
    puerta_tarjeta(&p, 2500, A, 4, true);
    VERIFICAR(p.armada);
    VERIFICAR(reportado(p, l, 2500) == LATIDO_ARMADA_S);
  }

  // El paso 6 del lazo: despues de reportar no repite el cambio ni se atrasa
  {
    puerta_t p;
    puerta_iniciar(&p, 0, true, false, PULSO_DEFECTO_MS, AVISO_DEFECTO_S);
    latido_t l;
    latido_iniciar(&l, puerta_latido_s(&p));
    VERIFICAR(latido_toca(&l, &p, 0));                 // el primero sale enseguida
    VERIFICAR(!latido_toca(&l, &p, 29999));
    VERIFICAR(latido_toca(&l, &p, 30000));             // cada 30 s armada
    puerta_tarjeta(&p, 31000, A, 4, true);             // se desarma
    reportado(p, l, 31000);
    VERIFICAR(l.intervalo_s == LATIDO_DESARMADA_S);
    VERIFICAR(!latido_toca(&l, &p, 31010));            // el reporte ya fue el latido: no sale otro en la misma vuelta
    VERIFICAR(!latido_toca(&l, &p, 31000 + 119999));
    VERIFICAR(latido_toca(&l, &p, 31000 + 120000));    // y de ahi cada 120 s
  }

  // El reporte lleva armada y abierta: la copia sigue a la puerta en cada vuelta del lazo
  {
    puerta_t p;
    puerta_iniciar(&p, 0, false, false, PULSO_DEFECTO_MS, AVISO_DEFECTO_S);
    latido_t l;
    latido_iniciar(&l, puerta_latido_s(&p));
    latido_toca(&l, &p, 0);
    VERIFICAR(!l.armada && !l.abierta);
    puerta_sensor(&p, 1000, true);                     // se abre
    latido_toca(&l, &p, 1010);
    VERIFICAR(!l.armada && l.abierta);
    puerta_sensor(&p, 2000, false);                    // se cierra
    puerta_tarjeta(&p, 3000, A, 4, true);
    puerta_tarjeta(&p, 4500, A, 4, true);              // doble pasada: arma
    VERIFICAR(p.armada);
    latido_toca(&l, &p, 4510);
    VERIFICAR(l.armada && !l.abierta);
    puerta_sensor(&p, 9000, true);                     // intrusion: armada y abierta
    latido_toca(&l, &p, 9010);
    VERIFICAR(l.armada && l.abierta);
  }

  // Arranca armada y abierta: el primer reporte ya lo dice (sin esperar a un cambio)
  {
    puerta_t p;
    puerta_iniciar(&p, 0, true, true, PULSO_DEFECTO_MS, AVISO_DEFECTO_S);
    latido_t l;
    latido_iniciar(&l, puerta_latido_s(&p));
    VERIFICAR(latido_toca(&l, &p, 0));
    VERIFICAR(l.armada && l.abierta);
  }

  RESUMEN();
}
