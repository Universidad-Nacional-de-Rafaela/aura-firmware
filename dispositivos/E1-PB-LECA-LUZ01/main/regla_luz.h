#pragma once
/*
 * Regla local de iluminacion del aula - E1-PB-LECA-LUZ01.
 *
 * Logica pura, sin HAL ni estado: la decide el firmware y se testea en host
 * (tests/test_regla.cpp). La toma el bucle del dispositivo en cada vuelta:
 *
 *   sistema en reposo (sin gente durante delay_off_s) -> luz apagada;
 *   con gente:
 *     BAJO < umbral_bajo_lx  en cualquiera de los dos sensores -> prender;
 *     ALTO > umbral_alto_lx  en los DOS sensores               -> apagar;
 *     en el medio, o con un sensor en falla                    -> mantener.
 *
 * Un sensor en falla lo decide el otro solo; si fallan los dos, se mantiene.
 * El anti-parpadeo (debounce de 2 s) lo lleva el dispositivo, no la regla.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clasifica un lux: 0 = BAJO, 1 = MEDIO, 2 = ALTO. */
static inline uint8_t regla_clasificar(float lux, uint32_t umbral_bajo_lx,
                                       uint32_t umbral_alto_lx)
{
    if (lux < (float)umbral_bajo_lx) return 0;
    if (lux > (float)umbral_alto_lx) return 2;
    return 1;
}

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

    uint8_t n_ventana = ok_ventana
        ? regla_clasificar(lux_ventana, umbral_bajo_lx, umbral_alto_lx) : 1;
    uint8_t n_aula = ok_aula
        ? regla_clasificar(lux_aula, umbral_bajo_lx, umbral_alto_lx) : 1;

    bool deseado = actual;
    if (ok_ventana && ok_aula) {
        if (n_ventana == 0 || n_aula == 0)      deseado = true;
        else if (n_ventana == 2 && n_aula == 2) deseado = false;
    } else if (ok_aula || ok_ventana) {
        uint8_t unico = ok_aula ? n_aula : n_ventana;
        if (unico == 0)      deseado = true;
        else if (unico == 2) deseado = false;
    }
    return deseado;
}

#ifdef __cplusplus
}
#endif
