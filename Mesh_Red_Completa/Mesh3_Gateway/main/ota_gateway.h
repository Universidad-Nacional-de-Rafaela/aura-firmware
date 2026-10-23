/*
 * Actualización por la malla, lado gateway. Ver ota_gateway.c y componentes/aura_mesh/include/aura_ota.h.
 *
 * Comando (hw/<hw_id>/command, para cualquier placa de la mesh, incluido el propio gateway):
 *   {"command":"ota","command_id":"...","params":{"url":"http://.../aura-x.bin","sha256":"<64 hex>"}}
 *   params.forzar_tipo = true: aunque la placa sea de otro tipo (cambio de uso, a propósito).
 *
 * response: transmitido (salió la oferta), recibido (el nodo tiene la imagen verificada y se
 * reinicia), aplicado (volvió con esa versión; details.firmware), rechazado (details.motivo).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

void ota_gw_iniciar(void);
/* Un comando "ota" de MQTT: lo encola o lo rechaza (publica la respuesta) */
void ota_gw_pedido(const uint8_t mac[6], const char *json);
/* Una trama OTA de un nodo (PIDO o FIN), desde la tarea de recepción: no bloquea */
void ota_gw_trama(const uint8_t mac[6], const uint8_t *datos, size_t largo);
/* El nodo mandó su reporte con fw = "tipo@versión": cierra una actualización que esperaba su vuelta */
void ota_gw_reporte(const uint8_t mac[6], const char *fw);
/* Conectado al broker: si el gateway se acaba de actualizar a sí mismo, publica cómo le fue */
void ota_gw_broker_conectado(void);
/* Para el status del gateway: lo que está haciendo (nada si no hay actualizaciones) */
void ota_gw_estado(cJSON *details);
