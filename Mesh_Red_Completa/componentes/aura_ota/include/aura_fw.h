/*
 * aura_fw: tipo y versión del firmware, y vuelta atrás de una actualización que no anda.
 *
 * El TIPO es el nombre del proyecto (project() en el CMakeLists.txt de la carpeta del proyecto) y
 * la VERSIÓN es PROJECT_VER: los dos quedan grabados en el binario (esp_app_desc_t) sin código
 * extra. Regla (GUIA_NODOS_HOJA.md, "Tipo y versión"): tipo "aura-<uso>" en minúsculas con a-z,
 * 0-9 y "-", hasta 31 caracteres; versión mayor.menor.parche. Todas las placas de un mismo uso
 * llevan el mismo tipo y el mismo binario.
 *
 * Vuelta atrás: una versión recién instalada arranca "sin confirmar". Queda confirmada cuando habló
 * (aura_fw_confirmar()) y además lleva AURA_FW_ESTABLE_S andando: una versión que habla y se cuelga
 * a los pocos segundos no tiene que quedar (probado 2026-10-22: se confirmaba y quedaba en un
 * reinicio sin fin que solo se arreglaba por USB). Si en AURA_FW_VERIFICAR_S no queda confirmada, o
 * si la placa se reinicia antes, el bootloader vuelve a la versión anterior. Un nodo habló cuando el
 * gateway lo ve volver con esa versión (trama OTA CONFIRMADA, no depende del broker) o con cualquier
 * EVENTO_OK; el gateway, cuando llega al broker.
 * Requiere CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (sdkconfig.defaults de cada proyecto).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define AURA_FW_VERIFICAR_S     600     /* sin confirmar en tanto tiempo -> vuelve a la anterior */
#define AURA_FW_ESTABLE_S       60      /* además de hablar, tiene que andar tanto sin colgarse */

/* Al arrancar: informa tipo y versión, revisa la regla y arranca la cuenta si hace falta */
void aura_fw_iniciar(void);
const char *aura_fw_tipo(void);
const char *aura_fw_version(void);
/* "aura-hoja-ejemplo@1.0.0": lo que va en el reporte ("fw") */
void aura_fw_texto(char *salida, size_t tam);
/* La versión nueva habla con la raíz (o el gateway con el broker): se confirma cuando además lleve
   AURA_FW_ESTABLE_S andando. Se puede llamar siempre (si no hay nada que confirmar, no hace nada). */
void aura_fw_confirmar(void);
bool aura_fw_sin_confirmar(void);
/* Lo que hay que hacer antes de volver a la versión anterior (por ejemplo, respaldar la cola en
   flash: la versión anterior la recupera al arrancar). NULL = nada. */
void aura_fw_antes_de_volver(void (*cb)(void));
