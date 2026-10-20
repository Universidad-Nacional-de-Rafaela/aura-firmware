/*
 * Plantilla de credenciales.h: copiala como credenciales.h y completá la red WiFi.
 * Credenciales del Gateway (router WiFi y broker MQTT).
 * Es el ÚNICO archivo que hay que tocar si cambia la red: ningún otro nodo de la
 * malla depende del WiFi. Después de cambiarlo se regraba solo el gateway.
 * El router tiene que emitir en 2,4 GHz: el ESP32 no usa 5 GHz.
 */
#pragma once

#define ROUTER_SSID     "CAMBIAR"
#define ROUTER_PASS     "CAMBIAR"

/*
 * Broker MQTT:
 *  ""  = buscarlo solo: el gateway pregunta por broadcast (UDP 18830) y broker.py
 *        le contesta con su IP. No hace falta saber la IP de la PC.
 *  "mqtt://IP:puerto" = usar siempre esa dirección (p. ej. un broker que no contesta
 *        el descubrimiento, como un Mosquitto o el servidor de AURA).
 */
#define BROKER_URI      ""
