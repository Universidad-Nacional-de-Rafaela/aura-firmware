/* Tests de host de entero_en_rango (main/config_validacion.h): make -C tests */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "config_validacion.h"

int main(void)
{
    int32_t v = -1;
    assert(entero_en_rango(300, 30, 3600, &v) && v == 300);
    assert(entero_en_rango(30, 30, 3600, &v) && v == 30);        /* bordes incluidos */
    assert(entero_en_rango(3600, 30, 3600, &v) && v == 3600);
    assert(entero_en_rango(0, 0, 600, &v) && v == 0);

    v = 77;                                                        /* si falla, *out no cambia */
    assert(!entero_en_rango(29, 30, 3600, &v) && v == 77);
    assert(!entero_en_rango(3601, 30, 3600, &v) && v == 77);
    assert(!entero_en_rango(2.5, 0, 600, &v) && v == 77);         /* no entero */
    assert(!entero_en_rango(1e300, 30, 3600, &v) && v == 77);     /* enorme: sin conversión indefinida */
    assert(!entero_en_rango(-1e300, 0, 600, &v) && v == 77);
    assert(!entero_en_rango(NAN, 0, 600, &v) && v == 77);
    assert(!entero_en_rango(INFINITY, 0, 600, &v) && v == 77);

    printf("test_config: OK\n");
    return 0;
}
