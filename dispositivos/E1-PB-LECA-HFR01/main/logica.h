#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t intervalo_s;
    uint32_t freezer_s;
    int16_t minimo[2];
    int16_t maximo[2];
} hfr_config_t;

typedef struct {
    uint32_t magic;
    uint32_t utc;
    int16_t temperatura[2];
    uint8_t validas;
    uint8_t activo;
    uint8_t reservado[2];
    uint32_t crc;
} hfr_corte_t;

typedef enum { HFR_SONDA_OK, HFR_SIN_RESPUESTA, HFR_FUERA_RANGO } hfr_sonda_estado_t;
hfr_config_t hfr_config_defecto(void);
bool hfr_config_valida(const hfr_config_t *cfg);
hfr_sonda_estado_t hfr_clasificar(float temperatura, bool leida);
bool hfr_fuera_umbral(float temperatura, int16_t minimo, int16_t maximo);
uint32_t hfr_crc32(const void *datos, size_t longitud);
bool hfr_corte_valido(const hfr_corte_t *corte);
void hfr_corte_sellado(hfr_corte_t *corte);
bool hfr_rtc_decodificar(const uint8_t registros[7], uint32_t *utc);
bool hfr_rtc_codificar(uint32_t utc, uint8_t registros[7]);
