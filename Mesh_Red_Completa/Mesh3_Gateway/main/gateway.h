/*
 * Lo que main.c le ofrece al resto del gateway (ota_gateway.c).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

/* hw/<mac>/response (§3.4). extra: campos que se agregan a details (puede ser NULL). */
bool gw_responder(const uint8_t mac[6], const char *status, const char *command_id, const char *motivo,
                  const cJSON *extra);
/* El "tipo@versión" que informó el nodo en su último reporte; false si no lo informó */
bool gw_fw_de_nodo(const uint8_t mac[6], char *fw, size_t tam);
/* Publica ya el status propio del gateway (para mostrar el avance de una actualización) */
void gw_publicar_status(void);
bool gw_mqtt_conectado(void);
const uint8_t *gw_mi_mac(void);
