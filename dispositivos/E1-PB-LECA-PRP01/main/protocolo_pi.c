#include "protocolo_pi.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

bool protocolo_pi_parsear(const char *linea, linea_pi_t *out)
{
    if (linea == NULL || out == NULL) {
        return false;
    }
    if (strcmp(linea, "U") == 0) {
        out->confiable = false;
        out->presencia = false;
        out->conf_pct = -1;
        return true;
    }
    if (linea[0] != 'P' || (linea[1] != '0' && linea[1] != '1')) {
        return false;
    }
    int conf = -1;
    if (linea[2] == ' ') {
        const char *num = &linea[3];
        size_t largo = strlen(num);
        if (largo == 0 || largo > 3) {
            return false;
        }
        for (const char *c = num; *c; c++) {
            if (!isdigit((unsigned char)*c)) {
                return false;
            }
        }
        conf = atoi(num);
        if (conf > 100) {
            return false;
        }
    } else if (linea[2] != '\0') {
        return false;
    }
    out->confiable = true;
    out->presencia = (linea[1] == '1');
    out->conf_pct = conf;
    return true;
}
