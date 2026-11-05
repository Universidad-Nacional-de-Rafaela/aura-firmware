#pragma once
#include "logica.h"
void hfr_energia_iniciar(bool nvs_disponible);
bool hfr_energia_actualizar(uint32_t utc, bool mesh_iniciada);
bool hfr_energia_bateria(void);
void hfr_energia_completar(const float temperaturas[2], uint8_t validas);
