#include "configuracion.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool fallo(char *motivo, size_t tam, const char *texto)
{
    if (tam) {
        snprintf(motivo, tam, "%s", texto);
    }
    return false;
}

bool hfr_config_parsear(const cJSON *params, const hfr_config_t *actual,
                       hfr_config_t *siguiente, char *motivo, size_t tam)
{
    if (!cJSON_IsObject(params) || !cJSON_GetArraySize(params)) {
        return fallo(motivo, tam, "sin_parametros");
    }
    hfr_config_t candidata = *actual;
    unsigned vistos = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, params) {
        const char *k = p->string;
        if (!k) {
            return fallo(motivo, tam, "parametro_invalido");
        }
        if (!strcmp(k, "aula")) {
            continue;
        }
        static const char *claves[] = {"intervalo_s", "temperatura_min_c", "temperatura_max_c"};
        /* Aceptar los nombres de umbral usados por las placas anteriores. */
        if (!strcmp(k, "hlo")) {
            k = claves[1];
        } else if (!strcmp(k, "hhi")) {
            k = claves[2];
        }
        int indice = -1;
        for (int i = 0; i < 3; ++i) {
            if (!strcmp(k, claves[i])) {
                indice = i;
                break;
            }
        }
        if (indice < 0) {
            return fallo(motivo, tam, "parametro_desconocido");
        }
        if (vistos & (1U << indice)) {
            return fallo(motivo, tam, "parametro_duplicado");
        }
        vistos |= 1U << indice;
        if (!cJSON_IsNumber(p) || !isfinite(p->valuedouble)) {
            return fallo(motivo, tam, "valor_no_numerico");
        }
        double valor = p->valuedouble;
        if (indice == 0) {
            if (valor < 5 || valor > 86400 || floor(valor) != valor) {
                return fallo(motivo, tam, "intervalo_fuera_de_rango");
            }
            candidata.intervalo_s = (uint32_t)valor;
        } else {
            if (valor < -55 || valor > 125) {
                return fallo(motivo, tam, "umbral_fuera_de_rango");
            }
            if (fabs(valor * 100 - round(valor * 100)) > 0.000001) {
                return fallo(motivo, tam, "umbral_mas_de_dos_decimales");
            }
            if (indice == 1) {
                candidata.minimo = (int16_t)lround(valor * 100);
            } else {
                candidata.maximo = (int16_t)lround(valor * 100);
            }
        }
    }
    if (!hfr_config_valida(&candidata)) {
        return fallo(motivo, tam, "umbrales_invertidos");
    }
    *siguiente = candidata;
    if (tam) {
        motivo[0] = '\0';
    }
    return true;
}

void hfr_config_a_json(const hfr_config_t *cfg, cJSON *objeto)
{
    cJSON_AddNumberToObject(objeto, "intervalo_s", cfg->intervalo_s);
    cJSON_AddNumberToObject(objeto, "temperatura_min_c", cfg->minimo / 100.0);
    cJSON_AddNumberToObject(objeto, "temperatura_max_c", cfg->maximo / 100.0);
}
