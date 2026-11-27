/* Firmware de banco: consola serie para probar motor, microswitch, BH1750 y modos automaticos sin malla. */
#pragma once
#include <stdbool.h>

void banco_correr(void);   /* no vuelve */
void banco_establecer_modo(int m);
int  banco_obtener_modo(void);
void banco_establecer_umbrales(double bajo, double alto, int conf_s);
void banco_obtener_umbrales(double *bajo, double *alto, int *conf_s, bool *luz_ok, double *lux);
