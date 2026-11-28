#pragma once
/*
 * Salida de las cerraduras: lo unico que toca los pines de los gates de los MOSFET. Cada
 * cerradura esta TRABADA sin corriente y se destraba solo mientras su MOSFET conduce (ADR-009,
 * decision 1).
 *
 * Igual que rele.c del modelo _modelos/PUE-RFID, una vez por locker: el corte de cada pulso lo
 * hace un esp_timer propio de ese locker, que NO depende de la tarea de la logica. Si la logica se
 * cuelga, el temporizador igual corta; si se cuelga todo, el watchdog reinicia la placa y los
 * pines vuelven a alta impedancia (con el pull-down de 10 kohm de cada gate, los MOSFET quedan
 * apagados). Ademas impone por su cuenta el tope duro de cada pulso.
 *
 * `locker` va de 0 a CANT_LOCKERS-1.
 */
#include <stdbool.h>
#include <stdint.h>

/* LO PRIMERO que se hace al arrancar: cada pin en 0 antes de configurarlo como salida. */
void salida_cerradura_reposo_inicial(void);

/* Crea los temporizadores de corte. Despues de salida_cerradura_reposo_inicial(). */
void salida_cerradura_iniciar(void);

/* Destraba la cerradura de un locker por `ms` (como mucho PULSO_TOPE_MS). Su temporizador la
   vuelve a trabar solo. */
void salida_cerradura_energizar(int locker, uint32_t ms);

/* Traba ya la cerradura de un locker. */
void salida_cerradura_soltar(int locker);

bool salida_cerradura_energizada(int locker);
