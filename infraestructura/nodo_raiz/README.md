# nodo_raiz

Raíz de la mesh ESP-WIFI-MESH de un edificio, y adaptador de la mesh en el sentido del contrato
([`docs/CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md), v4.0). Es una placa de la cátedra y la
**única** de la mesh que conoce el WiFi del edificio y el broker.

| Recibe | Publica |
|---|---|
| TELEMETRIA de una hoja | `hw/<hoja>/data` con `values`, `ingest_id`, `ts` y `adaptador` |
| REPORTE | `hw/<hoja>/status` (retain), con la configuración vigente y la cola |
| RESULTADO de un comando | `hw/<hoja>/response`: `recibido` y después `aplicado` o `rechazado` |
| ALERTA | `hw/<hoja>/alerts/<tipo>` |
| `hw/<hoja>/ack` de AURA | CONFIRMACION a la hoja, que recién ahí saca la muestra de su cola |
| `hw/<hoja>/command` de AURA | COMANDO a la hoja por la mesh, y `transmitido` o `rechazado` |

`<hoja>` es el `hw_id` de la placa: `mac-` y su MAC de fábrica en minúsculas. El raíz no tiene
tabla de nodos: publica todo lo que le llega y AURA decide a qué dispositivo corresponde.

Además publica su propio estado en `hw/<raíz>/status` cada 60 s (con LWT `offline`) e infiere
`offline` de una hoja que pasa 3 × su intervalo sin mandar nada.

## Puesta en marcha

1. `config_local.h` a partir de `config_local.h.example`: mesh, WiFi del edificio y broker. El
   broker de aura-app escucha en el puerto 1884 y no acepta anónimos: usuario `raiz` y la clave
   `MQTT_RAIZ_PASSWORD` del `.env` de aura-app. Si la clave está mal, el monitor serie dice
   `el broker rechazo usuario o clave`.
2. Placa ESP32-C3 SuperMini (`Nologo ESP32C3 Super Mini`, **USB CDC On Boot: Enabled**). Bibliotecas de `bibliotecas.txt`.
3. En el monitor serie: `con IP …` y `[MQTT] conectado`.

Sin backend, `herramientas/ack_falso.py --docker aura-mosquitto-1` contesta los `ack`
([`ack_falso.py`](../../herramientas/ack_falso.py)). Con la API de aura-app levantada no hace falta,
y no hay que usarlo: cada muestra recibiría dos `ack`.

Reemplaza a `nodo_gateway` (mesh ESP-NOW), que quedó en el tag `mesh-espnow-v2`.
