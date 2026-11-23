#include "../main/sensor_logica.h"
#include "../main/puerta_logica.h"
#include "../../../../comun/tests/aserciones.h"

#define BAJO 0
#define ALTO 1

int main() {
  // Antirrebote: un cambio de 30 ms es ruido; uno sostenido 50 ms es la puerta
  {
    sensor_filtro_t f;
    sensor_filtro_iniciar(&f, SENSOR_REED, BAJO, 0);            // reed en BAJO = cerrada
    bool ab = false;
    VERIFICAR(sensor_filtro_paso(&f, 100, ALTO, &ab) == SENSOR_IGUAL);
    VERIFICAR(sensor_filtro_paso(&f, 130, BAJO, &ab) == SENSOR_IGUAL);
    VERIFICAR(sensor_filtro_paso(&f, 200, ALTO, &ab) == SENSOR_IGUAL);
    VERIFICAR(sensor_filtro_paso(&f, 249, ALTO, &ab) == SENSOR_IGUAL);
    VERIFICAR(sensor_filtro_paso(&f, 250, ALTO, &ab) == SENSOR_CAMBIO);
    VERIFICAR(ab);
    VERIFICAR(sensor_filtro_paso(&f, 300, ALTO, &ab) == SENSOR_IGUAL);
  }

  // El boton de banco se lee al reves que el reed: BAJO = abierta
  {
    sensor_filtro_t f;
    sensor_filtro_iniciar(&f, SENSOR_BOTON, ALTO, 0);           // boton suelto = cerrada
    bool ab = true;
    VERIFICAR(sensor_filtro_paso(&f, 100, BAJO, &ab) == SENSOR_IGUAL);
    VERIFICAR(sensor_filtro_paso(&f, 150, BAJO, &ab) == SENSOR_CAMBIO);
    VERIFICAR(ab);
    VERIFICAR(sensor_filtro_paso(&f, 300, ALTO, &ab) == SENSOR_IGUAL);
    VERIFICAR(sensor_filtro_paso(&f, 350, ALTO, &ab) == SENSOR_CAMBIO);
    VERIFICAR(!ab);
  }

  // Armada y cerrada: un cambio sostenido es la puerta que se abrio, y eso si es intrusion
  {
    sensor_filtro_t f;
    sensor_filtro_iniciar(&f, SENSOR_REED, BAJO, 0);
    puerta_t p;
    puerta_iniciar(&p, 0, true, false, PULSO_DEFECTO_MS, AVISO_DEFECTO_S);
    bool ab = false;
    for (int64_t t = 100; t <= 400; t += 10) {
      if (sensor_filtro_paso(&f, t, ALTO, &ab) == SENSOR_CAMBIO) puerta_sensor(&p, t, ab);
    }
    VERIFICAR(p.intrusion);
  }

  // Arrancar con el sensor ya en "abierta" no dispara nada hasta que cambie
  {
    sensor_filtro_t f;
    sensor_filtro_iniciar(&f, SENSOR_REED, ALTO, 0);            // reed ALTO = abierta
    bool ab = false;
    VERIFICAR(sensor_filtro_paso(&f, 100, ALTO, &ab) == SENSOR_IGUAL);
    VERIFICAR(sensor_filtro_paso(&f, 1000, ALTO, &ab) == SENSOR_IGUAL);
  }

  RESUMEN();
}
