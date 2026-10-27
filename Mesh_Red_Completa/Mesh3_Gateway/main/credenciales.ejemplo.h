/*
 * Plantilla de credenciales.h: copiala como credenciales.h y completá las redes WiFi.
 * Credenciales del Gateway (router WiFi y broker MQTT).
 * Es el ÚNICO archivo que hay que tocar si cambia o se agrega una red: ningún otro nodo de la
 * malla depende del WiFi. Después de cambiarlo se regraba solo el gateway.
 * El router tiene que emitir en 2,4 GHz: el ESP32 no usa 5 GHz.
 */
#pragma once

/*
 * Redes WiFi que conoce el gateway, en ORDEN DE PREFERENCIA: {"SSID", "clave"}.
 * Al arrancar usa la primera que esté al alcance. Si se queda sin router 5 minutos,
 * se reinicia y prueba la siguiente. Tienen que emitir en 2,4 GHz.
 */
#define REDES_WIFI \
    { "RED_PREFERIDA",  "CAMBIAR" }, \
    { "OTRA_RED",       "CAMBIAR" }

/*
 * Broker MQTT (PRODUCCIÓN: siempre una dirección fija):
 *  "mqtts://servidor:8883" = TLS: el gateway verifica el certificado del broker antes de mandar nada.
 *        Con una CA propia, definir BROKER_CA_PEM (el certificado de la CA en PEM, como texto);
 *        sin ella se aceptan los certificados de las CA públicas.
 *  "mqtt://IP:puerto"      = sin TLS (solo en una red de confianza).
 *  ""  = buscarlo por broadcast (UDP 18830, broker.py contesta): SOLO EN EL BANCO, con
 *        DESCUBRIR_BROKER 1. A un broker descubierto nunca se le mandan usuario y clave.
 */
#define BROKER_URI      ""
#define DESCUBRIR_BROKER 0
/* #define BROKER_CA_PEM "-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
" */

/*
 * Usuario y clave del broker (solo se mandan a la BROKER_URI fija). Vacíos = anónimo.
 * El broker de aura-app no acepta anónimos: usuario "raiz" y la clave MQTT_RAIZ_PASSWORD de su .env.
 */
#define MQTT_USUARIO    ""
#define MQTT_CLAVE      ""

/* Actualización por la malla: solo https://. 1 = acepta también http:// (SOLO el banco, con ota.py) */
#define OTA_PERMITIR_HTTP 0
