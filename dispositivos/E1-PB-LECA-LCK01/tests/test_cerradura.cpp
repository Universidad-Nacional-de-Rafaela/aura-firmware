// Pulso de la cerradura con el tope de la FIT0620 (5 s) y el limite de 20 s por minuto
#include "../main/cerradura_pulso.h"
#include "../../../comun/tests/aserciones.h"

int main() {
  cerradura_t c;

  // Un pulso normal: se concede lo pedido y vence a tiempo
  cerradura_iniciar(&c);
  VERIFICAR(cerradura_pedir(&c, 1000, 3000, 3000) == 3000);
  VERIFICAR(c.energizada);
  VERIFICAR(!cerradura_vencio(&c, 3999));
  VERIFICAR(cerradura_vencio(&c, 4000));

  // El tope de la FIT0620 (5 s) no se pasa pidiendo mas, ni con un maximo mas alto
  VERIFICAR(PULSO_TOPE_MS == 5000);
  cerradura_iniciar(&c);
  VERIFICAR(cerradura_pedir(&c, 0, 15000, 20000) == 5000);

  // Cuatro pulsos de 5 s seguidos llenan los 20 s del minuto: el quinto no entra
  cerradura_iniciar(&c);
  int64_t t = 0;
  for (int i = 0; i < 4; i++) {
    VERIFICAR(cerradura_pedir(&c, t, 5000, 5000) == 5000);
    cerradura_cortar(&c, t + 5000);
    t += 5000;
  }
  VERIFICAR(cerradura_pedir(&c, t, 5000, 5000) == 0);
  // A los 60 s del primero ya se libero un tramo
  VERIFICAR(cerradura_pedir(&c, 65000, 5000, 5000) == 5000);

  // Un pulso cortado enseguida (puerta abierta) cuenta al menos PULSO_MINIMO_MS
  cerradura_iniciar(&c);
  cerradura_pedir(&c, 0, 3000, 3000);
  cerradura_cortar(&c, 100);
  VERIFICAR(cerradura_usado_ms(&c, 1000) == PULSO_MINIMO_MS);

  RESUMEN();
}
