# aura-firmware

> **Congelado desde el 2026-09-25.** Los primeros dispositivos de AURA no usan esta mesh: van por
> **LoRaWAN**, con ChirpStack (ver
> [`IC-lorawan-test`](https://github.com/Universidad-Nacional-de-Rafaela/IC-lorawan-test)). El
> firmware se conserva como referencia y como material de estudio de ESP-NOW. Implementa la
> versión 1.x del contrato MQTT de AURA (`enviado_a_mesh`, comandos de hasta 180 B), que la
> plataforma ya no acepta: conectarlo tal cual a AURA no funciona.
>
> `main` está protegida: los cambios entran solo por pull request.

Firmware de la malla de nodos ESP32 del proyecto **AURA** (Administración Unificada
de Recursos y Accesos), Universidad Nacional de Rafaela.

AURA busca responder a la demanda de soluciones tecnológicas que permitan gestionar
recursos, garantizar seguridad, optimizar consumos energéticos e hídricos y mejorar
la experiencia de estudiantes, docentes y personal administrativo.

## La malla

Tres roles de nodo, encadenados por ESP-NOW, con salida a MQTT y a la API por WiFi:

```
nodo_sensor  --ESP-NOW-->  nodo_sala  --ESP-NOW-->  nodo_gateway  --WiFi-->  MQTT / API
```

| Sketch | Rol |
|---|---|
| `firmware/nodo_sensor/` | Mide y emite lecturas hacia el nodo de sala |
| `firmware/nodo_sala/` | Reenvía lo de los sensores de su sala hacia el gateway |
| `firmware/nodo_gateway/` | Único nodo con WiFi: publica en MQTT y postea a la API |

Código compartido en `firmware/comun/` (protocolo, buffer circular, IDs de ingesta).

## Compilar

Arduino IDE, placa **XIAO_ESP32S3**, con **USB CDC On Boot: Enabled** (sin eso el
monitor serie queda mudo). Única biblioteca externa: **ArduinoMqttClient**, y solo
la usa el gateway.

Si los `#include` relativos a `../comun/` molestan, en `firmware/monolitico/` hay
copias autocontenidas de cada sketch, listas para pegar en el IDE. **No se editan
a mano**: se regeneran con `firmware/generar_monolitico.sh`.

## Configuración

Antes de flashear el gateway hay que completar en `firmware/nodo_gateway/nodo_gateway.ino`:

- `WIFI_SSID` / `WIFI_PASS` — la red a la que se conecta el gateway
- `MQTT_HOST` / `API_BASE` — dónde corren el broker y la API
- `TENANT_ID`, `GATEWAY_DEVICE_ID`
- `MAC_ESPERADA` y las MAC de los saltos vecinos — se leen con
  `firmware/utilidades/leer_mac/`

Los valores versionados son marcadores de posición. **No commitear credenciales reales.**

## Tests

Los headers de `comun/` se testean en la máquina de desarrollo, sin placa:

```bash
cd firmware/tests_host && make
```

## Utilidades

- `firmware/utilidades/broker.sh {ver|datos|estado|retenidos|limpiar|comando|log|problemas}`
  — inspección del broker Mosquitto sin instalar clientes MQTT locales.
- `firmware/utilidades/leer_mac/` — imprime la MAC de la placa por serie.

## Documentación

`docs/superpowers/` contiene el diseño de la malla, el plan de implementación y el
estado del banco de pruebas.
