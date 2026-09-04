# Estado del banco — cierre de jornada 2026-08-28

Punto de retomada para la próxima sesión de IC3 / firmware mesh de AURA.

## Dónde está todo

| Qué | Dónde |
|---|---|
| Firmware, spec y plan | worktree `aura-firmware`, rama `feature/firmware-mesh` |
| Repo principal, **sin tocar** | repo `aura`, rama `integration/frontend-mqtt` |
| Spike de descubrimiento (descartable) | carpeta local de la cátedra (fuera del repo) |

La rama `feature/firmware-mesh` **no está pusheada**. Nace de `main`, que solo tiene
`CLAUDE.md` y `README.md`: todo el proyecto real vive en `integration/frontend-mqtt`.
Hay que decidir contra qué rama va el PR.

## Las tres placas (XIAO ESP32S3)

| Etiqueta | MAC | Sketch | Alimentación |
|---|---|---|---|
| SENSOR | `E0:72:A1:F7:EF:E4` | `firmware/nodo_sensor/nodo_sensor.ino` | fuente, sin monitor |
| SALA | `E0:72:A1:F7:F5:48` | `firmware/nodo_sala/nodo_sala.ino` | USB |
| GATEWAY | `E0:72:A1:D8:48:B0` | `firmware/nodo_gateway/nodo_gateway.ino` | USB |

IDE: placa **XIAO_ESP32S3**, **USB CDC On Boot: Enabled** (sin esto el monitor queda mudo).
Única biblioteca externa: **ArduinoMqttClient** (solo la usa el gateway).
Si el include relativo a `../comun/` molesta, están las copias autocontenidas en
`firmware/monolitico/`, generadas por `firmware/generar_monolitico.sh`.

## Entorno

- WiFi: la red de pruebas del campus. Credenciales fuera del repo; se cargan en
  `WIFI_SSID` / `WIFI_PASS` de `nodo_gateway.ino`. No tiene aislamiento de
  clientes: verificado.
- Broker: contenedor **`aura-mosquitto`**, se levanta con
  `docker compose -f docker-compose.base.yml up -d mosquitto` desde la raíz del repo `aura`.
- **La notebook toma IP por DHCP: casi seguro cambió desde la última prueba.**
  Hay que verificarla con `ip -4 -o addr show wlp4s0` y actualizar `MQTT_HOST` y
  `API_BASE` en `nodo_gateway.ino`, y reflashear el gateway.
- Herramientas del broker: `firmware/utilidades/broker.sh {ver|datos|estado|retenidos|limpiar|comando|log|problemas}`.
  Los clientes MQTT están dentro de la imagen, no hace falta instalar nada.
- Utilidad para leer MACs: `firmware/utilidades/leer_mac/leer_mac.ino`.

## Lo que funciona

Cadena completa **sensor → sala → gateway → broker MQTT**, con datos variando de verdad.
El sensor mide la temperatura interna del chip (`temperatureRead()`, sin componentes) y
simula el lux con una rampa. El buffer demostró en hardware que no se pierden datos: al
conectar la sala, el sensor drenó ~19 muestras acumuladas sin perder ninguna.

Modos activos: `MODO_SIN_BACKEND 1` en el gateway (publica por MQTT, no usa la API) y
`MODO_BANCO 0` en la sala (sí sube al gateway).

## Lo que quedó a mitad

**Acción inmediata: reflashear las tres placas** con el último commit y leer el broker.
El último cambio (timeout de ACK de 1500 a 4000 ms + despacho más frecuente en el
gateway) **no se probó todavía**: es el experimento que confirma o descarta que los
duplicados vengan de latencia.

Cómo leerlo:
- `duplicados` quieto y `publicados` subiendo cada 10 s → hipótesis confirmada, cerrado.
- `duplicados` sigue subiendo → la latencia no era la causa. El siguiente sospechoso es
  la **carrera en el array `acks[]` del gateway**: lo escribe el callback (tarea de WiFi)
  y lo lee el `loop` (tarea principal) sin sincronización. Se dejó así a propósito para
  no cambiar dos cosas a la vez.

## Nunca probado

1. **La bajada de comandos de punta a punta.** El código está en los tres nodos pero no se
   ejercitó nunca. Se prueba con
   `firmware/utilidades/broker.sh comando 650e8400-e29b-41d4-a716-446655440001 '{"accion":"test"}'`
   y hay que ver los cinco pasos del log `[BAJA]` en la sala.
2. **La vía REST al backend.** `MODO_SIN_BACKEND 0` nunca se ejecutó: el backend de AURA
   no se levantó en toda la jornada.
3. **La prueba de idempotencia** (apagar el gateway 60 s y verificar que no hay filas
   duplicadas), que es la que justifica el diseño del `ingest_id`.
4. **El barrido de canales**, después de los arreglos.

## Pendientes de configuración

- `TENANT_ID` y los tres `device_id` de `nodo_gateway.ino` son **UUID de ejemplo**. Para
  MQTT alcanzan porque son solo nombres de tópico, pero el backend no va a poder resolver
  el dispositivo hasta que sean UUID reales de la tabla `devices`, con su `mac_address`.
- El spike de descubrimiento ESP-NOW quedó abierto: nunca se supo por qué NODO-A no
  recibía. Ya no bloquea nada, pero la respuesta sigue sin conocerse.

## Contexto que conviene no perder

Se encontraron **cinco defectos, todos en hardware, ninguno por los tests** (1.084
verificaciones de host pasaban). Están documentados con sus reglas en la **§9 del spec**,
`docs/superpowers/specs/2026-08-28-firmware-mesh-espnow-design.md`. El más instructivo es
el 9.1.E: el ACK iba al origen del dato en vez de al salto anterior, y era **invisible con
un solo salto** porque ahí origen y salto anterior coinciden.
