#include "logica.h"
#include <math.h>

hfr_config_t hfr_config_defecto(void)
{
    return (hfr_config_t){60, 300, {200, -2500}, {600, -1500}};
}

bool hfr_config_valida(const hfr_config_t *cfg)
{
    if (!cfg) {
        return false;
    }
    if (cfg->intervalo_s < 5 || cfg->intervalo_s > 86400 ||
        cfg->freezer_s < 5 || cfg->freezer_s > 86400) {
        return false;
    }
    for (int s = 0; s < 2; ++s) {
        if (cfg->minimo[s] < -5500 || cfg->maximo[s] > 12500 ||
            cfg->minimo[s] >= cfg->maximo[s]) {
            return false;
        }
    }
    return true;
}

hfr_sonda_estado_t hfr_clasificar(float temperatura, bool leida)
{
    if (!leida || !isfinite(temperatura) || temperatura == -127) {
        return HFR_SIN_RESPUESTA;
    }
    if (temperatura < -55 || temperatura > 125 || temperatura == 85) {
        return HFR_FUERA_RANGO;
    }
    return HFR_SONDA_OK;
}

bool hfr_fuera_umbral(float temperatura, int16_t minimo, int16_t maximo)
{
    return hfr_clasificar(temperatura, true) == HFR_SONDA_OK &&
        (temperatura * 100 < minimo || temperatura * 100 > maximo);
}

uint32_t hfr_crc32(const void *datos, size_t longitud)
{
    const uint8_t *p = datos;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < longitud; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
        }
    }
    return ~crc;
}

void hfr_corte_sellado(hfr_corte_t *corte)
{
    corte->magic = 0x48464331;
    corte->crc = hfr_crc32(corte, offsetof(hfr_corte_t, crc));
}

bool hfr_corte_valido(const hfr_corte_t *corte)
{
    if (corte->magic != 0x48464331 || corte->activo > 1 || corte->validas > 3 ||
        (corte->utc && corte->utc < 1700000000UL) ||
        corte->crc != hfr_crc32(corte, offsetof(hfr_corte_t, crc))) {
        return false;
    }
    for (int s = 0; s < 2; ++s) {
        if ((corte->validas & (1 << s)) &&
            hfr_clasificar(corte->temperatura[s] / 100.0f, true) != HFR_SONDA_OK) {
            return false;
        }
    }
    return true;
}

static int bcd(uint8_t valor)
{
    return (valor & 15) > 9 || (valor >> 4) > 9 ? -1 : (valor >> 4) * 10 + (valor & 15);
}

static uint8_t a_bcd(int valor)
{
    return (uint8_t)((valor / 10) * 16 + valor % 10);
}

bool hfr_rtc_decodificar(const uint8_t r[7], uint32_t *utc)
{
    int segundo = bcd(r[0]);
    int minuto = bcd(r[1]);
    int hora = bcd(r[2] & ((r[2] & 0x40) ? 0x1f : 0x3f));
    if (r[2] & 0x40) {
        if (hora < 1 || hora > 12) {
            return false;
        }
        hora = hora % 12 + ((r[2] & 0x20) ? 12 : 0);
    }
    int dia = bcd(r[4]);
    int mes = bcd(r[5] & 0x1f);
    int anio = bcd(r[6]);
    if ((r[0] & 0x80) || (r[1] & 0x80) || (r[2] & 0x80) ||
        (r[5] & 0x80) || segundo < 0 || segundo > 59 || minuto < 0 || minuto > 59 ||
        hora < 0 || hora > 23 || dia < 1 || mes < 1 || mes > 12 || anio < 0) {
        return false;
    }
    anio += 2000;
    static const int dias_mes[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int limite = dias_mes[mes - 1] + (mes == 2 && anio % 4 == 0);
    if (dia > limite) {
        return false;
    }
    uint32_t dias = 10957;
    for (int a = 2000; a < anio; ++a) {
        dias += 365 + (a % 4 == 0);
    }
    for (int m = 1; m < mes; ++m) {
        dias += dias_mes[m - 1] + (m == 2 && anio % 4 == 0);
    }
    *utc = (dias + dia - 1) * 86400UL + hora * 3600UL + minuto * 60UL + segundo;
    return *utc >= 1700000000UL;
}

bool hfr_rtc_codificar(uint32_t utc, uint8_t r[7])
{
    if (utc < 1700000000UL || utc >= 4102444800UL) {
        return false;
    }
    uint32_t dias = utc / 86400UL - 10957;
    int anio = 2000;
    while (dias >= (uint32_t)(365 + (anio % 4 == 0))) {
        dias -= 365 + (anio % 4 == 0);
        ++anio;
    }
    static const int dias_mes[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int mes = 1;
    while (dias >= (uint32_t)(dias_mes[mes - 1] + (mes == 2 && anio % 4 == 0))) {
        dias -= dias_mes[mes - 1] + (mes == 2 && anio % 4 == 0);
        ++mes;
    }
    r[0] = a_bcd(utc % 60);
    r[1] = a_bcd(utc / 60 % 60);
    r[2] = a_bcd(utc / 3600 % 24);
    r[3] = (uint8_t)((utc / 86400 + 4) % 7 + 1);
    r[4] = a_bcd(dias + 1);
    r[5] = a_bcd(mes);
    r[6] = a_bcd(anio - 2000);
    return true;
}
