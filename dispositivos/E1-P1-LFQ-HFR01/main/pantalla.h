#pragma once
#include "logica.h"
void hfr_pantalla_iniciar(void);
void hfr_indicadores(bool alarma, bool conectada);
void hfr_pantalla_actualizar(float temperatura, bool valida,
                            bool conectada, uint32_t pendientes, bool bateria,
                            uint32_t utc, bool oscura);
