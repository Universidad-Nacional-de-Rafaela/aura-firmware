#include "../main/regla_luz.h"
#include "../../../comun/tests/aserciones.h"

int main() {
    const uint32_t BAJO = 60, ALTO = 100;

    // Sin gente: siempre apagado.
    VERIFICAR(regla_deseado(false, true, 10, true, 10, true, BAJO, ALTO) == false);
    VERIFICAR(regla_deseado(false, true, 10, true, 10, false, BAJO, ALTO) == false);

    // Ventana alta: apaga (hay luz natural).
    VERIFICAR(regla_deseado(true, true, 200, true, 10, true, BAJO, ALTO) == false);
    VERIFICAR(regla_deseado(true, true, 200, true, 10, false, BAJO, ALTO) == false);

    // Ventana baja y foco ya prendido: mantiene (no apaga por la luz del propio foco).
    VERIFICAR(regla_deseado(true, true, 10, true, 300, true, BAJO, ALTO) == true);

    // Ventana baja, foco apagado y aula oscura: prende.
    VERIFICAR(regla_deseado(true, true, 10, true, 10, false, BAJO, ALTO) == true);

    // Ventana baja, foco apagado y aula iluminada: se queda apagado.
    VERIFICAR(regla_deseado(true, true, 10, true, 200, false, BAJO, ALTO) == false);

    // Ventana en el medio: histeresis.
    VERIFICAR(regla_deseado(true, true, 80, true, 10, true, BAJO, ALTO) == true);
    VERIFICAR(regla_deseado(true, true, 80, true, 10, false, BAJO, ALTO) == false);

    // Sin sensor de ventana: el aula solo puede prender.
    VERIFICAR(regla_deseado(true, false, 0, true, 10, false, BAJO, ALTO) == true);
    VERIFICAR(regla_deseado(true, false, 0, true, 10, true, BAJO, ALTO) == true);

    // Si fallan ambos sensores: mantiene.
    VERIFICAR(regla_deseado(true, false, 0, false, 0, true, BAJO, ALTO) == true);
    VERIFICAR(regla_deseado(true, false, 0, false, 0, false, BAJO, ALTO) == false);

    RESUMEN();
}
