#include "../main/config_validacion.h"
#include "../../../../comun/tests/aserciones.h"
#include <cmath>
#include <limits>

int main() {
  int32_t v = -7;
  // Dentro del rango y entero
  VERIFICAR(entero_en_rango(5000, 500, 10000, &v) && v == 5000);
  VERIFICAR(entero_en_rango(500, 500, 10000, &v) && v == 500);        // limites incluidos
  VERIFICAR(entero_en_rango(10000, 500, 10000, &v) && v == 10000);

  // Fuera del rango o no entero: false y la salida no se toca
  v = -7;
  VERIFICAR(!entero_en_rango(499, 500, 10000, &v) && v == -7);
  VERIFICAR(!entero_en_rango(10001, 500, 10000, &v) && v == -7);
  VERIFICAR(!entero_en_rango(5000.5, 500, 10000, &v) && v == -7);
  VERIFICAR(!entero_en_rango(-1, 500, 10000, &v) && v == -7);

  // Review: valores que no entran en int32. Se comparan como double ANTES de convertir
  // (convertir un double fuera del rango de int32 es comportamiento indefinido en C)
  VERIFICAR(!entero_en_rango(1e300, 500, 10000, &v));
  VERIFICAR(!entero_en_rango(-1e300, 500, 10000, &v));
  VERIFICAR(!entero_en_rango(2147483648.0, 500, 10000, &v));
  VERIFICAR(!entero_en_rango(-2147483649.0, -2147483647 - 1, 2147483647, &v));
  VERIFICAR(!entero_en_rango(std::numeric_limits<double>::infinity(), 500, 10000, &v));
  VERIFICAR(!entero_en_rango(-std::numeric_limits<double>::infinity(), 500, 10000, &v));
  VERIFICAR(!entero_en_rango(std::nan(""), 500, 10000, &v));
  VERIFICAR(v == -7);

  // Los extremos de int32 si valen cuando el rango los admite
  VERIFICAR(entero_en_rango(2147483647.0, 0, 2147483647, &v) && v == 2147483647);
  VERIFICAR(entero_en_rango(-2147483648.0, -2147483647 - 1, 0, &v) && v == (-2147483647 - 1));

  // aviso_s: 0 apaga el aviso; si no es 0, tiene que estar en el rango (5 a 600)
  v = -7;
  VERIFICAR(entero_en_rango_o_cero(0, 5, 600, &v) && v == 0);
  VERIFICAR(entero_en_rango_o_cero(-0.0, 5, 600, &v) && v == 0);
  VERIFICAR(entero_en_rango_o_cero(5, 5, 600, &v) && v == 5);
  VERIFICAR(entero_en_rango_o_cero(600, 5, 600, &v) && v == 600);
  v = -7;
  VERIFICAR(!entero_en_rango_o_cero(1, 5, 600, &v) && v == -7);      // entre 0 y el minimo: no
  VERIFICAR(!entero_en_rango_o_cero(4, 5, 600, &v) && v == -7);
  VERIFICAR(!entero_en_rango_o_cero(601, 5, 600, &v) && v == -7);
  VERIFICAR(!entero_en_rango_o_cero(-5, 5, 600, &v) && v == -7);
  VERIFICAR(!entero_en_rango_o_cero(0.5, 5, 600, &v) && v == -7);
  VERIFICAR(!entero_en_rango_o_cero(std::nan(""), 5, 600, &v) && v == -7);
  VERIFICAR(!entero_en_rango_o_cero(1e300, 5, 600, &v) && v == -7);

  RESUMEN();
}
