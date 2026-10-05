# Contrato MQTT — Proyecto AURA

> **Copia publicada para quienes escriben firmware.** La versión normativa vive en el repositorio
> privado `aura-app` (`docs/CONTRATO_MQTT.md`); esta copia se actualiza con cada versión del
> contrato. Si encontrás una diferencia, **manda la de `aura-app`**: avisá en un issue. Se omiten
> datos de red internos. Las rutas a archivos del backend (`app/backend/…`) son de `aura-app`.

**Estado**: normativo en `aura-app`. Ante una discrepancia con cualquier otro documento, manda el contrato; ante una discrepancia entre esta copia y la de `aura-app`, manda la de `aura-app`.
**Versión del contrato**: 3.0 (propuesta) · **Última actualización**: 2026-10-05
**Alcance**: mensajería entre los dispositivos IoT y la plataforma (backend FastAPI). Desde la
v3.0 hay **dos transportes**, cada uno con su adaptador
([ADR-003](adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md)):

- **Exterior o alejados → LoRaWAN**, a través de ChirpStack y del `lorawan-bridge`
  ([ADR-002](adr/ADR-002-lorawan-reemplaza-a-la-mesh-espnow.md)).
- **Interior → mesh ESP-NOW**, a través del **gateway de la mesh**.

Cubre la API REST de ingesta (§6) porque es el camino de la telemetría de la mesh.

Este documento se escribió **relevando el código**, no de memoria. Cada afirmación normativa
apunta al archivo y la función que la implementa hoy, o está marcada explícitamente como
**pendiente**. Las secciones marcadas ⚠️ describen desajustes reales entre las partes.

> **Estado de la v3.0.** Es una **propuesta**: ni el backend ni los adaptadores la implementan
> todavía. Lo que la v3.0 agrega al backend (estado `aplicado`, alertas especificadas) está
> marcado 🔧, igual que todo lo del `lorawan-bridge` y del gateway de la mesh, que **no
> existen** en su forma v3.0. La ingesta MQTT del backend **no persiste nada hoy** (§10,
> pendiente 1). La ingesta REST sí persiste, y es la que usa la mesh (§6).

---

## 0. Por qué existe este documento

El contrato MQTT es lo único que une el codec de un dispositivo LoRaWAN con una expresión
regular en Python corriendo en un contenedor. No hay compilador, ni tipos, ni tests que crucen
esa frontera: si un lado cambia el nombre de un tópico o un campo, el otro **no falla con un
error** — simplemente deja de reconocer el mensaje y lo descarta con un `logger.warning`. El
sistema sigue "funcionando" y los datos dejan de llegar.

LoRaWAN agrega una segunda frontera con el mismo modo de falla: casi todo lo que está mal
configurado en ChirpStack descarta los paquetes **sin una sola línea de log** (ver el
`README.md` del repo `IC-lorawan-test`). Es exactamente el modo de falla que la cátedra ya
evalúa en los TPs de los alumnos (ver `../../tps/CLAUDE.md`, sección de cruce
firmware↔simulador). Este documento es para que el proyecto cumpla lo que le pide a los
alumnos.

---

## 1. Brokers, adaptadores y conexión

Los dispositivos no hablan con AURA: hablan con un **adaptador**, que es el único que publica en
nombre de ellos. Hay uno por transporte:

```
nodos LoRaWAN ─► gateway LoRa ─► [broker LoRaWAN] ◄─► lorawan-bridge ──┐
                                 interno de ChirpStack                  ├─► [broker AURA] ◄─► backend
nodos mesh ─ESP-NOW─► (sala) ─ESP-NOW─► gateway de la mesh ─WiFi───────┤      este contrato
                                                            └─ REST ───┴─► POST /telemetry/ingest
```

| Broker | Qué circula | Quién lo usa |
|---|---|---|
| **LoRaWAN** (el Mosquitto del stack ChirpStack) | tópicos propios de ChirpStack (§2.2) | gateway-bridge, ChirpStack y el `lorawan-bridge`. **Ningún otro componente de AURA se suscribe acá.** |
| **AURA** | `devices/…`, `alerts/…`, `system/…` (§2.1) | los dos adaptadores y el backend |

| Adaptador | Telemetría (`data`) | Estado, comandos, respuestas, alertas |
|---|---|---|
| `lorawan-bridge` | MQTT, `devices/<id>/data` (§3.1) | MQTT |
| gateway de la mesh | **REST**, `POST /api/v1/telemetry/ingest` (§6). **No publica en `devices/<id>/data`** | MQTT |

La mesh usa REST para la telemetría porque el sensor guarda cada muestra hasta que AURA confirma
que la **persistió**, y esa confirmación solo la da la respuesta del REST. Un PUBACK de MQTT
solo dice que el broker recibió el mensaje. Publicar además en `data` duplicaría cada muestra
mientras la ingesta MQTT no deduplique por `ingest_id` (§5).

Parámetros del broker AURA:

| Parámetro | Valor | Dónde está definido |
|---|---|---|
| Host / puerto (backend) | `localhost:1883` por defecto | `MQTTService.__init__` en `app/backend/services/mqtt_service.py`. ⚠️ Nadie lo instancia con otro valor: la parametrización por entorno es parte del pendiente 1 de §10 |
| TLS | no | — |
| Autenticación | ninguna | — |
| Cliente backend | `paho-mqtt` | `app/backend/services/mqtt_service.py` |
| Cliente bridge | 🔧 pendiente | — |

⚠️ **Sin TLS ni credenciales.** Aceptable en la red de laboratorio; **no** en el despliegue
del campus. Cualquiera en la red puede publicar en `devices/+/command` y accionar
dispositivos, o en `devices/+/data` e inyectar mediciones. Ver §10, pendiente 11.

Del lado LoRaWAN la radio sí está cifrada: cada dispositivo tiene su propia clave AES (OTAA)
y el gateway no puede leer ni falsificar el contenido. La debilidad está del bridge para acá.

### Last Will and Testament 🔧

El bridge tiene su propio `device_id` en AURA (`BRIDGE_DEVICE_ID`) y registra un LWT antes de
conectar al broker AURA:

- **Tópico**: `devices/<BRIDGE_DEVICE_ID>/status`
- **Payload**: `{"status":"offline"}`
- **Retain**: `true` · **QoS**: `1`

Un `offline` retenido del bridge significa que **ningún** dispositivo LoRaWAN está llegando,
aunque cada uno siga apareciendo `online` con su último estado retenido. El backend debe
leerlo así.

El `offline` **de cada dispositivo** no puede venir de un LWT, porque el nodo no tiene
conexión MQTT propia: lo infiere el bridge (§3.2).

El **gateway de la mesh** hace lo mismo con su propio `device_id` (`MESH_GATEWAY_DEVICE_ID`) 🔧:
LWT en `devices/<MESH_GATEWAY_DEVICE_ID>/status`, `{"status":"offline"}`, retain, QoS 1. Si hay
varios gateways de mesh (uno por edificio), cada uno tiene su `device_id` y su LWT.

---

## 2. Árbol de tópicos

### 2.1 Broker AURA — lo que ve el backend

**Regla de oro: el `device_id` viaja en el tópico, no en el payload.** El backend lo extrae
con `match.group(1)` sobre la regex del tópico (`DeviceIntegrationService`, en
`app/backend/services/device_integration.py`). Un payload que traiga `device_id` adentro no es
un error, pero ese campo **se ignora**.

El `device_id` es un **UUID** de AURA, no el DevEUI del dispositivo ni un nombre legible.

```
devices/<device_id>/data        ← lorawan-bridge publica     → backend consume
devices/<device_id>/status      ← adaptador publica          → backend consume   [retain]
devices/<device_id>/command     ← backend publica            → adaptador consume
devices/<device_id>/response    ← adaptador publica          → backend consume
alerts/<device_id>/<tipo>       ← adaptador publica (§3.5)   → backend
system/health                   ← (sin emisor hoy)           → backend
```

"Adaptador" es el `lorawan-bridge` o el gateway de la mesh, según por dónde llegue el
dispositivo. Cada `device_id` pertenece a **un solo** adaptador.

**Sin prefijo `aura/`.** Los tópicos arrancan directamente en `devices/`. Esto contradice
otros documentos del repo; ver §8.

**El árbol no cambió respecto de la v1.x.** Cambió quién lo emite. Para el backend, cada
adaptador es un gateway que publica en nombre de muchos dispositivos, y no necesita saber por
qué transporte llegó cada uno.

Regexes exactas que reconoce el backend (`DeviceIntegrationService.__init__`):

```python
"device_status":   r"^devices/([^/]+)/status$"
"device_data":     r"^devices/([^/]+)/data$"
"device_command":  r"^devices/([^/]+)/command$"
"device_response": r"^devices/([^/]+)/response$"
"alert":           r"^alerts/([^/]+)/([^/]+)$"
"system_health":   r"^system/health$"
```

Un tópico que no matchee ninguna se descarta con `logger.warning(f"Topic no reconocido")`
y `process_mqtt_message` devuelve `False`. **No hay excepción ni reintento.**

### 2.2 Broker LoRaWAN — lo que consume el bridge 🔧

Tópicos de la integración MQTT de ChirpStack v4 (JSON, `json=true` en `chirpstack.toml`):

```
application/<appId>/device/<devEUI>/event/up       uplink con el payload ya decodificado
application/<appId>/device/<devEUI>/event/join     el dispositivo hizo OTAA
application/<appId>/device/<devEUI>/event/status   batería y margen del enlace
application/<appId>/device/<devEUI>/event/txack    el gateway transmitió un downlink
application/<appId>/device/<devEUI>/event/ack      el nodo acusó un downlink confirmado
application/<appId>/device/<devEUI>/event/log      errores de ChirpStack sobre el device
```

El bridge se suscribe a `application/<appId>/device/+/event/+`, con **un solo `appId`**: el de
la aplicación `aura`. Con esa suscripción cubre todos los dispositivos, presentes y futuros.

Los tópicos `<región>/gateway/<gatewayEUI>/…` (gateway-bridge ↔ ChirpStack) son internos del
stack LoRaWAN y **nadie de AURA los toca**.

### 2.3 Identidad: de DevEUI a `device_id` 🔧

ChirpStack identifica por DevEUI; AURA, por UUID. El puente entre ambos es un **tag del
dispositivo en ChirpStack**:

```
aura_device_id = <uuid del dispositivo en AURA>
```

ChirpStack incluye los tags del dispositivo en cada evento (`deviceInfo.tags`), así que el
bridge traduce sin tabla propia ni consulta a la base: el mapeo vive donde se da de alta el
dispositivo.

| Concepto de ChirpStack | Qué es en AURA |
|---|---|
| Tenant | uno: la UNRaf |
| Application | una: `aura` |
| Device profile | un **tipo** de nodo: codec, versión LoRaWAN, clase A/C, límite de downlink (§3.3) |
| Device | cada placa, con su tag `aura_device_id` |

**Agregar un dispositivo no requiere tocar ni el bridge ni el backend**: se lo da de alta en
ChirpStack con su tag y en AURA con el mismo UUID.

Un evento de un dispositivo **sin** `aura_device_id` se descarta con `logger.warning` y suma a
un contador de huérfanos. **No se publica en ningún tópico de AURA**, ni siquiera con el DevEUI
en lugar del UUID: publicarlo rompería la regla de que el `device_id` es un UUID de AURA.

Para la bajada (§3.3) el bridge necesita el mapeo inverso, UUID → DevEUI. Lo arma con los tags
que ve en los uplinks y, si un UUID no aparece, lo consulta a la API de ChirpStack.

### 2.4 Identidad en la mesh: de MAC a `device_id` 🔧

En la mesh, el nodo se identifica por la **MAC** de su placa. El gateway de la mesh tiene una
**tabla MAC → UUID** en su configuración local (no versionada), equivalente al tag
`aura_device_id` de ChirpStack.

- Una trama de una MAC que no está en la tabla se descarta con un log y suma a un contador de
  huérfanos. **No se publica** con la MAC en lugar del UUID.
- Agregar un sensor de mesh requiere agregarlo a la tabla y reflashear el gateway (ADR-003, *Lo
  que esta decisión NO resuelve*).

### 2.5 Un dispositivo es una placa

En los dos transportes, **un `device_id` es una placa**, no una sonda. Un nodo con varias
sondas publica un solo `device_id` y distingue cada sonda por el nombre del campo en `values`
(§3.1). Los parámetros que valen para todo el nodo (intervalo de recuperación, canal de radio)
se configuran sobre ese único `device_id`.

Se descartó un `device_id` por sonda porque los parámetros del nodo quedarían repetidos en varios
dispositivos de AURA. Cambiarlos desde uno modificaría los otros sin que AURA lo registre.

---

## 3. Mensajes

### 3.1 `devices/<device_id>/data` — telemetría

**Emisor**: `lorawan-bridge`, uno por cada `event/up` con payload decodificado · **QoS 1** ·
**retain `false`**. El gateway de la mesh **no** publica acá: su telemetría entra por REST (§6),
con el mismo contenido en `payload` que tendría `values`.

```json
{ "values": { "temp_c": 21.5, "lux": 123.4 },
  "ingest_id": "5f0c1b1e-8a6d-4a55-9f2b-7c3e2d1a0b99" }
```

| Campo | Obligatorio | Tipo | Notas |
|---|---|---|---|
| `values` | **sí** | objeto | El campo `object` del uplink, tal como lo devolvió el codec del device profile. Único campo requerido (`_validate_data_payload`). Sin él el mensaje se descarta. |
| `ingest_id` | no | UUID | El `deduplicationId` que ChirpStack asigna al uplink (§5). ⚠️ El backend **todavía no lo lee** por MQTT. |
| `unit` | no | objeto | El backend lo lee (`_process_device_data`); ningún emisor lo manda. Default `{}`. |
| `quality` | no | entero | Leído, nadie lo manda. Default `100`. |

**`values` es opaco para el bridge.** Su forma la define el codec de cada device profile, no
el bridge. Un codec nuevo no requiere cambiar el bridge.

**Solo mediciones.** En `values` no van RSSI, SNR, contadores de trama ni nada que describa el
enlace o el estado del nodo: eso va a `status` (§3.2). Mezclarlos fue el problema de `pend` y
`desc` en la v1.x: un panel que graficaba "todas las variables del dispositivo" graficaba el
buffer de la mesh como si fuera un sensor.

Un uplink **sin** `object` (el device profile no tiene codec, o el codec falló) **no se
publica en `data`**. El bridge lo registra con `logger.warning`: publicar los bytes crudos en
base64 dentro de `values` metería basura en la serie temporal.

**Varias sondas, un mensaje.** Un nodo con varias sondas (§2.5) las distingue por el nombre del
campo: `{"temp_heladera_c": 4.5, "temp_freezer_c": -18.0}`. Si las sondas tienen intervalos
distintos, cada mensaje lleva solo las que se midieron: `values` no tiene que traer todas.

**Una sonda que falla no se publica como medición.** Su campo **no aparece** en `values`: ni
un valor centinela (`327.67`, `-127`, `85`), ni `null`, ni la temperatura interna del
microcontrolador. La falla se informa por `alerts/<device_id>/sensor` (§3.5). Si ninguna sonda
del mensaje dio un valor válido, no se publica `data`.

**Hora de la medición: `ts` 🔧.** Campo opcional, ISO 8601 en UTC
(`"2026-10-05T14:03:00Z"`). Para LoRaWAN es el `time` del uplink en ChirpStack. Para la mesh es
la hora del RTC del nodo, y va en el campo `ts` del evento REST (§6). Si el nodo no tiene hora
válida, **se omite** y vale la hora de llegada: no se inventa.

⚠️ **El backend todavía no lee `ts` por MQTT.** `_process_device_data` usa la hora de llegada.
Con uplinks cada pocos minutos el error es chico; con una cola retenida tras una caída (§5) deja
de serlo. El REST sí lo lee.

### 3.2 `devices/<device_id>/status` — estado del dispositivo

**Emisor**: adaptador · **QoS 1** · **retain `true`** ← distinto de `data`

El bridge publica un `status` por cada uplink, join o evento de estado de ChirpStack. Todo lo
que no es `status` va **anidado bajo `details`**, que es lo que el backend persiste:

```json
{
  "status": "online",
  "details": {
    "evento": "up", "rssi": -87, "snr": 6.5, "gateway_eui": "24e124fffefafc09",
    "fcnt": 142, "dr": 5, "f_port": 1
  }
}
```

| Campo | Obligatorio | Notas |
|---|---|---|
| `status` | **sí** | Único requerido (`_validate_status_payload`). Valores: `"online"`, `"offline"`. |
| `details` | no | El backend lee `payload.get("details", {})` en `_process_device_status`. |

Qué pone el bridge en `details` según el evento de ChirpStack 🔧:

| Evento | `details` |
|---|---|
| `event/up` | `evento: "up"`, `rssi` y `snr` del mejor gateway, `gateway_eui`, `fcnt`, `dr`, `f_port` |
| `event/join` | `evento: "join"` |
| `event/status` | `evento: "status"`, `bateria` y `margen` tal como los informa ChirpStack |

Qué pone el gateway de la mesh en `details` 🔧. Lo publica cuando el nodo manda su reporte de
estado (al arrancar, al reconectar y después de aplicar una configuración):

| Campo | Notas |
|---|---|
| `evento` | `"reporte"` |
| `transporte` | `"mesh"` (el bridge pone `"lorawan"`) |
| `config` | la configuración **vigente** en el nodo, tal como la tiene guardada (§3.4) |
| `pendientes`, `descartadas` | muestras en la cola del nodo y muestras perdidas por cola llena |
| `alimentacion` | `"red"`, `"bateria"` o `"desconocida"` |
| `rssi` | del último salto ESP-NOW, si el gateway lo tiene |

El contador de la cola y la alimentación van acá y **no** en `values`, por la misma razón que
`pend` y `desc` en la v1.x.

**`offline` por inferencia 🔧.** Como no hay LWT por dispositivo, el adaptador publica
`{"status":"offline","details":{"motivo":"sin_uplinks"}}` (retain) cuando un dispositivo pasa
`3 ×` su intervalo de uplink sin transmitir. En LoRaWAN el intervalo sale del campo *uplink
interval* del device profile, que ChirpStack también usa para marcar el dispositivo como
inactivo. En la mesh sale de la configuración vigente que reportó el nodo. El siguiente
mensaje lo vuelve a `online`.

`retain: true` es deliberado: un backend que arranca después recibe el último estado conocido
de cada dispositivo de inmediato, sin esperar el próximo uplink, que en LoRaWAN pueden ser
minutos.

### 3.3 `devices/<device_id>/command` — comando hacia el dispositivo

**Emisor**: backend · **Consumidor**: el adaptador del dispositivo · **QoS 1**

```json
{ "command": "turn_on", "params": { "canal": 1 }, "command_id": "c-2026-0001" }
```

| Campo | Obligatorio | Notas |
|---|---|---|
| `command` | **sí** | `_validate_command_payload` |
| `params` | no | Default `{}` |
| `command_id` | no | 🔧 Si viene, el bridge lo devuelve en cada `response` de ese comando (§3.4). Sin él, las respuestas no se pueden correlacionar con el comando. |

**Configuración remota.** Todo dispositivo que tenga parámetros (intervalo, umbrales locales,
calibración) los recibe con el comando `set_config`:

```json
{ "command": "set_config", "params": { "intervalo_s": 300 }, "command_id": "c-2026-0002" }
```

`params` lleva **solo los parámetros que cambian**. El nodo valida cada valor contra su rango
antes de aplicarlo, lo guarda en memoria no volátil y, si algo no valida, **no aplica nada** del
comando. Un comando mal armado no puede dejar el nodo inutilizable.

**Camino del comando en LoRaWAN 🔧.** El bridge busca el DevEUI del `device_id` (§2.3) y encola el
downlink con la **API de ChirpStack** (`EnqueueDeviceQueueItem`), pasando el JSON como
`object` para que lo codifique el `encodeDownlink` del device profile. Se usa la API y no el
tópico `…/command/down` porque la API devuelve el `id` del ítem encolado, y ese `id` es lo que
permite atar el `txack` y el `ack` posteriores a este comando.

**Límite de tamaño 🔧.** El límite real es en **bytes en el aire después del codec** y depende
del *data rate*. Por eso **no lo valida el backend** (quitado de `_process_device_command` en
la v2.0): el backend no puede saberlo. Lo valida el bridge contra el tag
`aura_max_downlink_bytes` del **device profile**. Si el comando no entra, o el profile no tiene
ese tag, el bridge **no lo encola** y publica `rechazado` en `response` con el motivo. Ningún
comando se descarta sin respuesta.

**Camino del comando en la mesh 🔧.** El gateway de la mesh busca la MAC del `device_id`
(§2.4), convierte el JSON a la trama binaria de la mesh y la manda por ESP-NOW, pasando por el
nodo de sala si lo hay. El límite es el **payload de la trama, 180 B**, después de codificar.
Si no entra, o el `command` no existe para ese tipo de nodo, el gateway publica `rechazado` con
el motivo. Tampoco acá se descarta un comando sin respuesta.

**Latencia.** En la mesh el nodo escucha todo el tiempo: el comando llega en menos de un
segundo. En LoRaWAN, un dispositivo **clase A** solo abre la ventana de recepción después de
transmitir: el comando sale con su próximo uplink, que puede tardar minutos. Los dispositivos
que tienen que ejecutar un comando en segundos (actuadores de iluminación, cerraduras) van en
**clase C**, que escucha todo el tiempo y es viable porque están alimentados de red. La clase
es un atributo del device profile, no del comando.

### 3.4 `devices/<device_id>/response` — confirmación

**Emisor**: adaptador · **QoS 1** · **retain `false`**

```json
{ "status": "transmitido", "details": { "command_id": "c-2026-0001", "queue_item_id": "…" } }
```

| `status` | Qué significa | LoRaWAN 🔧 | Mesh 🔧 |
|---|---|---|---|
| `encolado` | el comando quedó en una cola esperando salir | respuesta de `EnqueueDeviceQueueItem` | no se usa: la mesh no encola |
| `transmitido` | salió por radio | `event/txack` | `esp_now_send` aceptó la trama para el primer salto |
| `recibido` | el nodo acusó recibo | `event/ack` con `acknowledged: true` (solo downlinks confirmados) | el nodo devolvió el resultado del comando |
| `aplicado` | **el nodo informó que lo ejecutó** | uplink de reporte del nodo (ver abajo) | resultado del nodo con `aplicado = 1` |
| `rechazado` | no salió, el nodo no lo acusó, o el nodo no lo aplicó | error de la API, tamaño excedido (§3.3), `acknowledged: false`, o reporte del nodo con rechazo | tamaño excedido, MAC sin destino, o resultado del nodo con rechazo |

El motivo de un `rechazado` va en `details.motivo`.

**`aplicado` es el único estado que confirma ejecución**, y es nuevo en la v3.0. Lo publica el
adaptador **solo** cuando el firmware lo informa explícitamente, con el `command_id`. Junto con
`aplicado`, `details.config` trae la configuración que quedó vigente en el nodo, para que el
backend la compare con lo que pidió. Los otros cuatro estados siguen sin confirmar ejecución:
`recibido` solo dice que el comando llegó a la radio del nodo.

En LoRaWAN, el nodo informa la ejecución en un uplink en un puerto propio (el codec decide cuál
y lo traduce a `{ "command_id": …, "aplicado": true|false, "config": {…} }`). Como el nodo
clase A solo puede transmitir en su turno, `aplicado` llega con algún uplink posterior: hasta
entonces, la interfaz muestra el cambio como **pendiente**.

🔧 Hoy el backend acepta solo `encolado`, `transmitido`, `recibido` y `rechazado`
(`RESPONSE_STATES` en `DeviceIntegrationService`) y registra `confirma_ejecucion: False`
siempre. La v3.0 requiere agregar `aplicado` y registrar `confirma_ejecucion: True` en ese
estado. `enviado_a_mesh` de la v1.x sigue rechazado.

### 3.5 `alerts/<device_id>/<tipo>` y `system/health`

**Emisor de `alerts`**: adaptador · **QoS 1** · **retain `false`**

```json
{ "severity": "high", "message": "Sonda freezer sin respuesta",
  "details": { "campo": "temp_freezer_c", "motivo": "sin_respuesta" },
  "ts": "2026-10-05T14:03:00Z" }
```

| Campo | Obligatorio | Notas |
|---|---|---|
| `severity` | no | `info`, `warning`, `high`, `critical`. Default `unknown` (`ALERT_SEVERITIES`) |
| `message` | no | texto para una persona |
| `details` | no | objeto; depende del tipo |
| `ts` | no | como en §3.1 |

Tipos especificados en la v3.0 🔧. El tipo va en el tópico:

| Tipo | Cuándo | `details` |
|---|---|---|
| `sensor` | una sonda dejó de responder o da un valor fuera de su rango físico | `campo` (el nombre que tendría en `values`) y `motivo`: `sin_respuesta`, `fuera_de_rango` |
| `energia` | el nodo pasó a batería o volvió a la red | `alimentacion`: `"bateria"` o `"red"` |

El adaptador publica la alerta **una vez por cambio**, no en cada medición: una sonda que
falla durante una hora es una alerta, no sesenta. Cuando la sonda vuelve, se publica otra con
`severity: "info"` y `motivo: "recuperada"`.

Las alertas por **umbral** (temperatura fuera del rango de conservación) **no** las emite el
adaptador: las calcula AURA a partir de `values`. Un LED de alarma local en el nodo es
independiente de eso.

**`system/health`**: declarado en las regexes del backend, **sin emisor**. Reservado.

---

## 4. Handlers del despacho

| Handler | Tópico que lo dispara | Estado |
|---|---|---|
| `_process_device_status` | `devices/<id>/status` | ✅ llama al Device Service para persistir (ver ⚠️ de §10.1) |
| `_process_device_data` | `devices/<id>/data` | ✅ llama al Device Service para persistir (ver ⚠️ de §10.1) |
| `_process_device_command` | `devices/<id>/command` | ✅ valida y registra en memoria |
| `_process_device_response` | `devices/<id>/response` | ✅ valida y registra en memoria |
| `_process_alert` | `alerts/<id>/<tipo>` | ✅ valida y registra en memoria |
| `_process_system_health` | `system/health` | ✅ valida y registra en memoria |

Los cuatro últimos **solo validan y registran en memoria** (`last_command`, `last_response`,
`last_alert`, `last_system_health`). Ese estado se pierde al reiniciar el proceso.

Validaciones que aplican:

| Tópico | Requiere | Rechaza |
|---|---|---|
| `devices/<id>/command` | `command` | — (el tamaño lo valida el bridge, §3.3) |
| `devices/<id>/response` | `status` | `status` fuera de `RESPONSE_STATES` (🔧 la v3.0 agrega `aplicado`) |
| `alerts/<id>/<tipo>` | — (`severity` default `unknown`) | `severity` fuera de `{info, warning, high, critical, unknown}` |
| `system/health` | `status` | — |

Además, `process_mqtt_message` descarta con `warning` cualquier payload que no sea un objeto
JSON, antes de despachar.

---

## 5. Entrega, duplicados e idempotencia

**Todo el sistema usa QoS 1: "al menos una vez".** El broker puede entregar el mismo mensaje
más de una vez, así que todo consumidor debe ser idempotente.

| Capa | Mecanismo | Alcance |
|---|---|---|
| Radio → ChirpStack | ChirpStack junta las recepciones del mismo uplink por varios gateways y emite **un** evento con un `deduplicationId` | varios gateways, un solo dato |
| Nodo → ChirpStack | uplinks confirmados con reintentos (firmware de `IC-lorawan-test`) | ⚠️ cómo trata ChirpStack un reintento con el mismo `fCnt` está **sin verificar en banco** |
| Bridge → AURA | `ingest_id` = `deduplicationId` en `data` 🔧 | sobrevive a reinicios del bridge: el id lo genera ChirpStack, no el bridge |
| Nodo mesh → gateway de la mesh | el nodo **guarda cada muestra** en memoria no volátil y la reenvía hasta recibir la confirmación de AURA 🔧 | sobrevive a cortes del gateway, del WiFi y de AURA, dentro de la capacidad de la cola del nodo |
| Gateway de la mesh → AURA | `ingest_id` generado **en el nodo**, un UUID por muestra, guardado junto con ella antes del primer envío 🔧 | igual en cada reintento y después de reiniciar cualquier placa |
| Backend (REST) | `ingest_id` con `unique=True` en `ts_telemetry` (`models/telemetry.py`) | deduplicación real en base |
| Backend (MQTT) | ⚠️ **ninguno**: `_process_device_data` no lee `ingest_id` | pendiente 6 de §10 |

En la mesh el `ingest_id` lo genera el nodo y no el gateway. La v1.x lo derivaba en el gateway
a partir de `(MAC, boot_id, seq)`; con una cola persistente en el nodo eso ya no alcanza, porque
la misma muestra puede reenviarse después de reiniciar el gateway.

**Confirmación en la mesh.** El gateway de la mesh confirma una muestra al nodo **solo** cuando
la respuesta del REST dice que quedó persistida (§6). Hasta entonces el nodo la conserva. El
ACK de radio de ESP-NOW, el ACK del nodo de sala y el ACK del gateway **no** son confirmación
de persistencia.

### Mensajes durante una caída ⚠️

MQTT no tiene colas: el broker entrega a quien esté suscripto **en ese momento**. Para que
guarde los mensajes QoS 1 mientras un consumidor está caído, el consumidor tiene que conectar
con un **`client_id` fijo y sesión persistente** (`clean_session=False`).

Hoy el backend hace lo contrario: `client_id=f"aura-mqtt-service-{uuid4().hex[:8]}"`, distinto
en cada arranque (`MQTTService._create_client`). **Cada reinicio del backend es un hueco en la
serie.** Lo mismo vale para el bridge del lado de ChirpStack: si se cae, pierde uplinks que
LoRaWAN no retransmite.

Si algún día hay más de una réplica del backend, la suscripción pasa a ser compartida
(`$share/aura-backend/devices/+/data`), para que cada mensaje lo procese una sola réplica.

---

## 6. Ingesta REST: el camino de la telemetría de la mesh

`POST /api/v1/telemetry/ingest` (`app/backend/api/endpoints/telemetry.py`) es el único camino
que hoy **persiste y deduplica** en base. Desde la v3.0 es por donde entra la telemetría de la
mesh (ADR-003).

```json
{ "events": [ {
    "tenant_id": "…", "device_id": "<uuid de la placa>", "type": "temperatura",
    "ts": "2026-10-05T14:03:00Z", "ingest_id": "<uuid generado en el nodo>",
    "payload": { "temp_heladera_c": 4.5, "temp_freezer_c": -18.0 } } ] }
```

- `payload` sigue las mismas reglas que `values` en §3.1: solo mediciones, y una sonda que
  falla no aparece.
- `ts` es la hora del RTC del nodo; si no es válida, se omite.
- Nada de diagnóstico del nodo en `payload`: eso va a `status` (§3.2) y a `alerts` (§3.5).

**Respuesta.** `201` con `{"inserted", "duplicates", "errors", "message"}` (`TelemetryBatchResponse`).
Los tres primeros son **enteros**. El gateway de la mesh considera persistido un evento si y solo
si:

```
HTTP 201  y  errors == 0  y  inserted + duplicates == cantidad de eventos enviados
```

Un duplicado cuenta como persistido: es la misma muestra, que ya estaba en la base.

⚠️ **Dos defectos del endpoint**, a corregir antes de mandar más de un evento por request:

1. **Un evento con error hace perder los anteriores del mismo batch.** Ante una excepción el
   endpoint hace `db.rollback()`, que descarta también los eventos ya insertados con `flush()`
   pero todavía sin `commit()`. Esos eventos **igual se cuentan** en `inserted`.
2. **Devuelve `201` aunque todos los eventos hayan fallado.** Hay que mirar `errors`, no el
   código HTTP.

Mientras no se corrijan, **el gateway de la mesh manda un evento por request**: con un solo
evento, el primer defecto no puede perder nada.

La integración HTTP de ChirpStack, que podría apuntar a este endpoint, sigue descartada para
LoRaWAN: postea el formato de eventos de ChirpStack y no el de AURA
([ADR-002](adr/ADR-002-lorawan-reemplaza-a-la-mesh-espnow.md)).

---

## 7. Versionado y evolución

El mensaje MQTT **no lleva ninguna marca de versión**. `AURA_PROTO_VERSION` de la mesh versiona
la **trama ESP-NOW entre nodos**, no el mensaje MQTT: los tres roles de la mesh (sensor, sala,
gateway) tienen que usar la misma versión de trama, y eso no lo ve AURA.

Reglas de evolución de este contrato:

1. **Agregar un campo opcional** al payload no rompe nada: no requiere subir versión.
2. **Renombrar o eliminar un campo, o cambiar su tipo, unidad o conjunto de valores
   válidos**, es incompatible: subir la versión mayor de este documento y coordinar el
   despliegue.
3. **Cambiar el árbol de tópicos** es lo más caro: los dispositivos ya instalados siguen
   publicando en el viejo. Si pasa, el backend debe consumir ambos durante la transición.
4. Todo cambio incompatible se anota en §9.

En los dos transportes la regla 3 es barata para los nodos, porque los tópicos de AURA los arma
el adaptador y no el firmware del nodo. Con LoRaWAN se cambian sin reflashear nada; con la mesh
hay que reflashear **solo el gateway**. Lo que sí queda grabado en los nodos
es el **formato de sus bytes**, que interpreta el codec del device profile. Un cambio de
formato en el firmware requiere un device profile nuevo, no editar el codec del existente.

**Pendiente**: agregar un campo `v` al payload MQTT para que el backend pueda distinguir
generaciones de emisores sin adivinar por la forma del JSON.

---

## 8. ⚠️ Documentos y código que contradicen este contrato

**Este documento es el normativo**; los demás están desactualizados y hay que corregirlos:

| Documento | Qué dice | Estado |
|---|---|---|
| **Este documento** | `devices/<id>/data` por el `lorawan-bridge`; telemetría de la mesh por REST | ✅ el árbol coincide con el backend; `aplicado` y los adaptadores están pendientes |
| `docs/adr/ADR-002-…` | "la mesh queda congelada" | ⏸ reemplazado en ese punto por [ADR-003](adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) |
| `docs/CONTRATOS_APIS.md` | `devices/+/data`, `devices/+/status`, `alerts/#` | ✅ coincide |
| `docs/Arquitectura_IoT_AURA.md:74-108` | `aura/devices/<id>/sensors/<tipo>/data` | ❌ jerarquía profunda que no existe |
| `CLAUDE.md` | remite a este documento | ✅ |
| `.cursorrules` | `devices/<device_id>/{data,status,command,response}`, remite a este documento | ✅ |
| `recursos/circutor_reader/mqtt_messages.json` | `aura/devices/circutor_…/telemetry` | ❌ mock data con otro esquema |
| `firmware/` de este repo y el repo público `aura-firmware` | mesh ESP-NOW v1.x: telemetría por `devices/<id>/data`, `enviado_a_mesh`, `ingest_id` derivado en el gateway, sin cola persistente en el nodo | ❌ **a actualizar a la v3.0** (ADR-003). Hasta entonces, ningún gateway de la mesh se conecta al broker de producción |
| `firmware/README.md` y los avisos de `docs/superpowers/` sobre la mesh | "congelada" | ❌ a actualizar: la mesh volvió para interior |

**El caso Circutor sigue abierto** y la v2.0 lo vuelve más claro. El medidor se integra por
Modbus TCP en `<IP interna>:502`, su lector vive en el repositorio propio `aura-circutor`
(ADR-001 de `aura-app`, no publicado) y todavía no publica en
ningún lado. Con el `lorawan-bridge` y el gateway de la mesh, el patrón queda establecido: **un adaptador por
protocolo que publica en `devices/<uuid>/…`**. La opción recomendada de ADR-001 (que el
Circutor adopte `devices/<device_id>/data`) es exactamente ese patrón. Mientras no se decida,
sus mensajes se descartan: `aura/devices/circutor_<IP>/telemetry` no matchea ninguna regex
de §2.1.

---

## 9. Historial de cambios

| Versión | Fecha | Cambio |
|---|---|---|
| 3.0 | 2026-10-05 | **Propuesta. Dos transportes** ([ADR-003](adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md)): mesh ESP-NOW en interior y LoRaWAN en exterior, cada uno con su adaptador sobre el mismo árbol. **Incompatible**: nuevo estado `aplicado` en `response`, que confirma ejecución y el backend todavía rechaza; un `device_id` por placa y no por sonda (§2.5); una sonda que falla no aparece en `values`. Nuevos: §2.4 (tabla MAC → UUID), telemetría de la mesh por REST con criterio de persistencia (§6), campo `ts` (§3.1), `set_config` (§3.3), alertas `sensor` y `energia` (§3.5), `details` de la mesh en `status`, LWT del gateway de la mesh. Documentados dos defectos del endpoint REST (§6). |
| 2.0 | 2026-09-25 | **Los dispositivos pasan a LoRaWAN** ([ADR-002](adr/ADR-002-lorawan-reemplaza-a-la-mesh-espnow.md)). El árbol de tópicos no cambia; cambia el emisor: el `lorawan-bridge` reemplaza al gateway de la mesh. **Incompatible**: `response` pasa a `encolado`/`transmitido`/`recibido`/`rechazado` (se rechaza `enviado_a_mesh`); el backend deja de limitar el comando a 180 B, porque el límite pasa al bridge por device profile. Nuevos: §2.2 (tópicos de ChirpStack), §2.3 (tag `aura_device_id`), `ingest_id` en `data`, `details` de radio en `status`, `command_id`. Quedan sin objeto los campos `pend`, `desc` y `lux_sim` y el modo REST del gateway. |
| 1.2 | 2026-09-25 | §8: `CLAUDE.md` y `.cursorrules` alineados con este contrato; el lector Circutor ya vive en `aura-circutor`. |
| 1.1 | 2026-09-06 | Implementados los cuatro handlers faltantes de §4, con validación de longitud de comando y de severidad de alerta, y guardia de payload no-dict. |
| 1.0 | 2026-09-06 | Primera redacción, relevada del código en `main` tras la consolidación de ramas. Documenta el estado tal cual está, incluidos los desajustes conocidos. |

---

## 10. Pendientes

Ordenados por impacto:

1. ⚠️ **Que la ingesta MQTT persista algo.** Hoy no persiste nada: `mqtt_service.start()` no
   se llama nunca, `_on_message` crea tareas asyncio desde un thread sin event loop, y los dos
   endpoints del Device Service a los que llama `device_integration` no existen o esperan otro
   esquema. El diagnóstico completo y el plan están en
   `docs/superpowers/plans/2026-09-08-ingesta-mqtt-p0.md`. **Bloquea todo lo demás**: sin
   esto, el bridge publicaría al vacío.
2. **Corregir los dos defectos del endpoint REST** (§6): que un evento con error no haga perder
   los anteriores del batch, y que la respuesta no sea `201` si hubo errores. Es lo que más
   rápido destraba la mesh, porque su telemetría entra por ahí.
3. **Aceptar `aplicado` en `RESPONSE_STATES`** con `confirma_ejecucion: True` (§3.4).
4. **Actualizar el firmware de la mesh a la v3.0** (§8): telemetría por REST con el criterio
   de §6, `ingest_id` generado y guardado en el nodo, cola persistente, `set_config` con
   resultado del nodo, tabla MAC → UUID, alertas de §3.5.
5. **Implementar el `lorawan-bridge`** según §1-§5 (todo lo marcado 🔧).
6. **Que el backend lea `ingest_id` y `ts` por MQTT** y deduplique igual que el camino REST
   (§3.1, §5).
7. **Sesión persistente con `client_id` fijo** en backend y adaptadores (§5), para no perder los
   mensajes que lleguen durante un reinicio.
8. **Cifrado de ESP-NOW** (PMK/LMK) en la mesh, y su gestión de claves (ADR-003).
9. **Persistir `command`, `response`, `alerts` y `system/health`** (§4): hoy solo viven en
   memoria.
10. **Verificar en banco** qué hace ChirpStack con un uplink confirmado reintentado con el mismo
    `fCnt` (§5).
11. **Credenciales y ACL en el broker AURA** (§1): que solo los adaptadores puedan publicar en
    `devices/+/status`, `response` y `alerts/#`, solo el bridge en `devices/+/data`, y solo el
    backend en `devices/+/command`. Autenticación del endpoint REST para el gateway de la mesh.
12. **Corregir los documentos de §8** para que dejen de contradecir al código.
13. **Un test de contrato**: publicar los payloads de ejemplo de este documento y verificar que
    `process_mqtt_message` los acepta, y lo mismo con el REST. Es lo único que evita que este
    documento se desactualice como los de §8.

De la v1.x quedan resueltos por la v3.0 el destino de los diagnósticos del gateway (van a
`status.details`) y `pend`/`desc` (pasan a `pendientes`/`descartadas` en `status`). `lux_sim`
sigue sin objeto. `TENANT_ID` y los UUID del firmware de la mesh van en su configuración local,
no versionada.
