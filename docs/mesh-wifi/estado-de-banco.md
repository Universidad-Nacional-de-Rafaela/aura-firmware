# Estado de banco — mesh ESP-WIFI-MESH

**Primera prueba en placa: 2026-10-09**, con tres **ESP32-C3 SuperMini** (la placa de la mesh; la
XIAO ESP32S3 queda para LoRaWAN), FQBN `esp32:esp32:nologo_esp32c3_super_mini`, el hotspot de un
celular como router (canal 1) y el broker de aura-app en la notebook. Lo que sigue es el protocolo
de prueba, ordenado por cuántas placas hacen falta. Cada prueba anota su resultado acá.

En la mesh ESP-NOW aparecieron **cinco defectos que solo se vieron en hardware**, pese a más de
mil verificaciones de host que pasaban ([`../historico/mesh-v1/`](../historico/mesh-v1/), §9).
Hay que esperar lo mismo.

## Defectos que aparecieron en placa (2026-10-09)

1. **Hoja o relevo sin `MESH_ROUTER_SSID`**: `esp_mesh_set_config` devuelve
   `ESP_ERR_MESH_ARGUMENT` y la placa reiniciaba en bucle. El SSID es obligatorio (la clave no).
   Ahora `radio_iniciar` lo avisa claro, la hoja sigue midiendo y guardando, y el relevo no arranca.
2. **`WiFi.h` de Arduino reconecta por su cuenta** (autoReconnect, prendido por defecto) y se pelea
   con la mesh: cientos de `MESH_EVENT_PARENT_DISCONNECTED` por segundo (motivos 106 `SCAN_FAIL` y
   3), el raíz nunca vuelve a asociarse y el canal queda saturado (la notebook, en el mismo canal,
   perdió el WiFi). Arreglo: `WiFi.setAutoReconnect(false)`.
3. **SSID y clave guardados en la NVS** (por un `WiFi.begin` anterior en la misma placa) producen lo
   mismo. Arreglo: `WiFi.persistent(false)` antes de `WiFi.mode()`. Probado flasheando el raíz sobre
   una NVS con credenciales.

Si una placa vieja quedó con la tormenta, se la silencia dejándola en el bootloader:
`esptool --port <puerto> --after no-reset read-mac`.

## Preparación

- Placa **ESP32-C3 SuperMini** (`nologo_esp32c3_super_mini`, CDC por defecto). Cada placa con su `hw_id` anotado en una
  etiqueta ([`herramientas/leer_mac`](../../herramientas/leer_mac/)).
- Un broker en la notebook y `herramientas/ack_falso.py` contra él. El raíz tiene que llegar a la
  notebook por la red (ojo con el aislamiento de clientes en las redes de invitados).
- `config_local.h` en cada sketch a partir de su `.example`. Mismo `MESH_ID`, `MESH_CLAVE` y
  `MESH_CANAL` en todas las placas; el canal es el del WiFi al que se asocia el raíz.

## Con 1 placa (raíz)

| # | Prueba | Qué se espera | Resultado |
|---|---|---|---|
| 1.1 | Flashear `nodo_raiz` y abrir el monitor serie | `[MESH] raiz iniciado`, después `[RAIZ] con IP …` | ✅ 2026-10-09: con IP en 6-8 s |
| 1.2 | Broker | `[MQTT] conectado y suscripto a hw/+/ack y hw/+/command`; en el broker, `hw/mac-<raíz>/status` retenido con `rol: "raiz"` | ✅ 2026-10-09 |
| 1.3 | `broker.sh comando mac-aabbccddeeff '{"command":"led"}'` (una MAC que no está) | `rechazado` con `nodo_no_alcanzable` en `hw/mac-aabbccddeeff/response` | ✅ 2026-10-09 |
| 1.4 | Desenchufar el raíz | el broker publica el LWT: `hw/mac-<raíz>/status` → `offline` | pendiente |
| 1.5 | Dejarlo una hora | `status` cada 60 s, sin reinicios; anotar la RAM libre | pendiente |

La 1.1 y la 1.2 son el riesgo (c) de la spec: que `WiFi.h` de Arduino y `esp_mesh` convivan en el
raíz (DHCP solo en el raíz, sockets de `WiFiClient` sobre la interfaz de la mesh). Si fallan, se
reabre [ADR-006](../adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md); la alternativa
es el cliente MQTT de ESP-IDF (`esp-mqtt`, incluido en el core).

## Con 2 placas (raíz + hoja)

| # | Prueba | Qué se espera | Resultado |
|---|---|---|---|
| 2.1 | Hoja con `sensor_ejemplo`, **sin** `MESH_ROUTER_SSID` (variante A) | se une a la mesh; en el raíz llega su REPORTE → `hw/mac-<hoja>/status` | ❌ 2026-10-09: `ESP_ERR_MESH_ARGUMENT` (defecto 1) |
| 2.2 | Si 2.1 falla: con `MESH_ROUTER_SSID` y sin clave (variante B) | ídem | ✅ 2026-10-09: se une en capa 2 en ~3 s |
| 2.3 | Si 2.2 falla: con SSID y clave del router (variante C) | ídem; **si solo anda así, se reabre ADR-006** (las hojas llevarían la clave del WiFi del edificio) | pendiente |
| 2.4 | Telemetría con `ack_falso.py` en modo `ok` | `hw/mac-<hoja>/data` → `ack` `persistido` → en la hoja, `AURA confirmo …, quedan 0` | ✅ 2026-10-09 |
| 2.5 | `ack_falso.py` en `sin_ack` 3 minutos, después `ok` | la hoja acumula y reintenta (15 s, 30 s, 1 min…); al volver, se vacía y los reintentos salen `duplicado` | pendiente |
| 2.6 | `set_config {"intervalo_s": 30}` | `transmitido`, `recibido`, `aplicado` con `config`; nuevo `status` | ✅ 2026-10-09 |
| 2.7 | `actuador_ejemplo` y el comando `led` | el LED prende; `aplicado` | pendiente |
| 2.8 | Reiniciar el raíz con la hoja andando | la hoja se reengancha sola y reporta; nada se pierde | ✅ 2026-10-09 con los arreglos 2 y 3: raíz con IP en 7-25 s, la hoja se reengancha en ~80 s y vacía la cola sin perder nada (3 reinicios). Antes: ❌ |
| 2.9 | Apagar la hoja más de 3 × su intervalo | el raíz publica `offline` con `motivo: "sin_tramas"`; al volver, `online` | pendiente |
| 2.10 | `esp_mesh_get_type()` en la hoja | `MESH_LEAF`: no acepta hijos | ✅ 2026-10-09: la hoja informa tipo hoja |

## Con 3 placas (raíz + relevo + hoja)

| # | Prueba | Qué se espera | Resultado |
|---|---|---|---|
| 3.1 | Hoja lejos del raíz y cerca del relevo (otro piso) | la hoja cuelga del relevo (capa 3); el raíz publica con la **MAC de la hoja**, no la del relevo | pendiente |
| 3.2 | Comando a la hoja de 3.1 | llega por el relevo sin configurar rutas | pendiente |
| 3.3 | Apagar el relevo | la hoja guarda en su cola; al volver el relevo, se reengancha y vacía la cola | pendiente |

## Medir el aire

Con los relevos andando, contar los beacons por segundo en el canal (por ejemplo con `airodump-ng`
o el analizador de WiFi de un teléfono) y anotarlo: es el costo de ADR-006 sobre el WiFi del
campus. Antes de instalar en el edificio, hablarlo con Sistemas.
