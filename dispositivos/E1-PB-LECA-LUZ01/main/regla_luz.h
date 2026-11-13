#pragma once
/*
 * Regla local de iluminacion del aula - E1-PB-LECA-LUZ01.
 *
 * Logica pura, sin HAL ni estado. La decide el firmware:
 *
 *   sin gente (reposo, tras delay_off_s) -> luz apagada;
 *   con gente (manda la ventana, porque el sensor del aula ve el foco):
 *     ventana > umbral_alto_lx                      -> apagar (hay luz natural);
 *     ventana < umbral_bajo_lx:
 *        foco ya encendido                         -> mantener (aunque el aula suba por el foco);
 *        foco apagado y aula < umbral_alto_lx      -> prender;
 *        foco apagado y aula >= umbral_alto_lx     -> mantener apagado;
 *     ventana entre los dos umbrales                -> mantener (histeresis);
 *   sin sensor de ventana: el aula solo puede prender; apaga la falta de gente;
 *   si fallan los dos sensores de luz: mantiene.
 *
 * El anti-parpadeo (debounce de 2 s) lo lleva el dispositivo, no la regla.
 * Test de host: tests/test_regla.cpp.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Estado deseado de la luz. `actual` es el estado encendido actual, para
 * mantenerlo cuando no hay senal clara. Los lux en falla se pasan con su
 * `ok_*` en false.
 */
static inline bool regla_deseado(bool sistema_activo, bool ok_ventana,
                                 float lux_ventana, bool ok_aula, float lux_aula,
                                 bool actual, uint32_t umbral_bajo_lx,
                                 uint32_t umbral_alto_lx)
{
    if (!sistema_activo) return false;

    if (ok_ventana) {
        if (lux_ventana > (float)umbral_alto_lx) return false;

        if (lux_ventana >= (float)umbral_bajo_lx) return actual;

        if (actual) return true;
        if (!ok_aula) return true;
        return lux_aula < (float)umbral_alto_lx;
    }

    if (ok_aula && !actual && lux_aula < (float)umbral_bajo_lx) return true;
    return actual;
}

#ifdef __cplusplus
}
#endif
