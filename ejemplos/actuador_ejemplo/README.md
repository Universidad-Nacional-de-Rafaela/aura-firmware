# actuador_ejemplo

Ejemplo de **actuador** sobre la mesh ESP-WIFI-MESH: acciona el LED de la XIAO ESP32S3 (GPIO 21)
con un comando propio, además de `set_config`. No necesita hardware extra.

| Desde AURA (`hw/<hw_id>/command`) | Qué hace | Respuesta (`hw/<hw_id>/response`) |
|---|---|---|
| `{"command":"led","params":{"encendido":true},"command_id":"c-1"}` | prende el LED | `recibido`, `aplicado` |
| `{"command":"led","params":{"encendido":"si"}}` | nada | `rechazado`, `encendido_no_es_booleano` |
| `{"command":"set_config","params":{"intervalo_s":30}}` | cambia el intervalo | `aplicado`, con la config vigente |

El estado del LED se publica como medición, `led_encendido` (0 o 1), cada `intervalo_s` y cada
vez que cambia.

En banco, con el raíz y [`herramientas/ack_falso.py`](../../herramientas/ack_falso.py):

```bash
herramientas/broker.sh comando mac-<MAC de la placa> '{"command":"led","params":{"encendido":true},"command_id":"c-1"}'
herramientas/broker.sh respuestas
```

Para el ejemplo más chico (una medición), ver [`sensor_ejemplo`](../sensor_ejemplo/).
