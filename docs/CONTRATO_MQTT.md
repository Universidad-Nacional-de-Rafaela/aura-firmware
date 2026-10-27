# Contrato MQTT — Proyecto AURA

> **Copia publicada para quienes escriben firmware.** La versión normativa vive en el repositorio
> privado `aura-app` (`docs/CONTRATO_MQTT.md`); esta copia se actualiza con cada versión del
> contrato. Si encontrás una diferencia, **manda la de `aura-app`**: avisá en un issue. Se omiten
> datos de red internos. Las rutas a archivos del backend (`app/backend/…`) y a sus planes
> (`docs/superpowers/…`) son de `aura-app`.

**Estado**: normativo en `aura-app`. Ante una discrepancia con cualquier otro documento, manda el contrato; ante una discrepancia entre esta copia y la de `aura-app`, manda la de `aura-app`.
**Versión del contrato**: 4.4 (propuesta) · **Última actualización**: 2026-10-26
**Alcance**: mensajería entre los dispositivos IoT y la plataforma (backend FastAPI). Hay **dos
transportes**, cada uno con su adaptador
([ADR-003](adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md)):

- **Exterior o alejados → LoRaWAN**, a través de ChirpStack y del `lorawan-bridge`
  ([ADR-002](adr/ADR-002-lorawan-reemplaza-a-la-mesh-espnow.md)).
- **Interior → mesh ESP-WIFI-MESH**, a través del **raíz de la mesh**
  ([ADR-006](adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md)).

Lo que define la v4.0:

- **Identidad por placa** ([ADR-005](adr/ADR-005-identidad-por-placa-y-mapeo-en-aura.md)): los
  adaptadores publican con el `hw_id` de la placa en un árbol `hw/<hw_id>/…`, y AURA traduce el
  `hw_id` a su dispositivo. El UUID de AURA no cruza la frontera con los dispositivos.
- **MQTT es el único camino de entrada**, y el backend confirma lo que guardó en
  `hw/<hw_id>/ack` ([ADR-004](adr/ADR-004-mqtt-punto-comun-de-ingesta-con-ack-de-persistencia.md)).

Este documento se escribió **relevando el código**, no de memoria. Cada afirmación normativa
apunta al archivo y la función que la implementa hoy, o está marcada explícitamente como
**pendiente** (🔧). Las secciones marcadas ⚠️ describen desajustes reales entre las partes.

> **Estado de la v4.0** (2026-10-09). **El backend la implementa** (`app/backend/ingesta/`):
> escucha el árbol `hw/`, guarda por `devices.hw_id` o en cuarentena, deduplica por `ingest_id`,
> publica el `ack` después del commit y guarda comandos, respuestas y alertas. **El
> `lorawan-bridge` también** (`servicios/lorawan_bridge/`): la subida desde el 2026-10-09 y la
> bajada de comandos desde el 2026-10-14. Los dos están probados con los brokers reales del banco,
> y la bajada contra el ChirpStack del banco con un device de prueba; de punta a punta con un nodo
> real todavía no, porque ningún firmware ni codec del banco recibe comandos. El raíz de la mesh la implementa en
> `aura-firmware` (`infraestructura/nodo_raiz/`), **compilado y sin probar en placa** (pendiente
> 17). La v3.1 (MQTT con `ack` sobre `devices/<uuid>/…`) no se publicó: quedó absorbida acá.

---

## 0. Por qué existe este documento

El contrato MQTT es lo único que une el codec de un dispositivo LoRaWAN, o el firmware de una
hoja de la mesh, con una expresión regular en Python corriendo en un contenedor. No hay
compilador, ni tipos, ni tests que crucen esa frontera: si un lado cambia el nombre de un tópico
o un campo, el otro **no falla con un error** — simplemente deja de reconocer el mensaje y lo
descarta con un `logger.warning`. El sistema sigue "funcionando" y los datos dejan de llegar.

LoRaWAN agrega una segunda frontera con el mismo modo de falla: casi todo lo que está mal
configurado en ChirpStack descarta los paquetes **sin una sola línea de log** (ver el
`README.md` del repo `IC-lorawan-test`). Es exactamente el modo de falla que la cátedra ya
evalúa en los TPs de los alumnos. Este documento es para que el proyecto cumpla lo que le pide a los
alumnos.

---

## 1. Brokers, adaptadores y conexión

Los dispositivos no hablan con AURA: hablan con un **adaptador**, que es el único que publica en
nombre de ellos. Hay uno por transporte:

```
nodos LoRaWAN ─► gateway LoRa ─► [broker LoRaWAN] ◄─► lorawan-bridge ──┐
                                 interno de ChirpStack                  ├─► [broker AURA] ◄─► backend ─► base
hojas ─ESP-WIFI-MESH─► concentrador del aula ─ESP-WIFI-MESH─► raíz de la mesh ─WiFi┘   este contrato
```

| Broker | Qué circula | Quién lo usa |
|---|---|---|
| **LoRaWAN** (el Mosquitto del stack ChirpStack) | tópicos propios de ChirpStack (§2.2) | gateway-bridge, ChirpStack y el `lorawan-bridge`. **Ningún otro componente de AURA se suscribe acá.** |
| **AURA** | `hw/…` (§2.1) | los dos adaptadores y el backend |

| Adaptador | Su `hw_id` | Telemetría | Usa el `ack` (§3.6) | Estado, comandos, respuestas, alertas |
|---|---|---|---|---|
| `lorawan-bridge` | `svc-lorawan-bridge` | `hw/eui-<DevEUI>/data` | no: el nodo LoRaWAN no guarda muestras | MQTT |
| raíz de la mesh | `mac-<MAC del raíz>` | `hw/mac-<MAC de la hoja>/data` | **sí**: confirma a la hoja solo con el `ack` | MQTT |

**Por qué hay un `ack`.** La hoja de la mesh guarda cada muestra hasta que AURA confirma que la
recibió. Un PUBACK de MQTT solo dice que el broker recibió el mensaje, no que el backend lo
guardó. Por eso el backend publica en `hw/<hw_id>/ack` **después del commit** en la base, y el
raíz le confirma la muestra a la hoja recién entonces
([ADR-004](adr/ADR-004-mqtt-punto-comun-de-ingesta-con-ack-de-persistencia.md)).

Parámetros del broker AURA:

| Parámetro | Valor | Dónde está definido |
|---|---|---|
| Host / puerto (backend) | `MQTT_BROKER_HOST` / `MQTT_BROKER_PORT`: `mosquitto:1883` dentro del compose, `localhost:1884` desde el host (el 1883 del host es del Mosquitto de ChirpStack) | `app/backend/core/env.py`, `docker-compose.yml` |
| TLS | no | — |
| Autenticación | usuario y clave por cliente, sin anónimos: `aura-backend`, `lorawan-bridge`, `raiz`, `monitor` (solo lectura) | `config/mosquitto/` (`mosquitto.conf`, `acl`, `arrancar.sh`); claves en `.env` (`MQTT_*_PASSWORD`) |
| ACL | los adaptadores (`lorawan-bridge`, `raiz`) publican `data`, `status`, `response` y `alerts` y leen `command`; el raíz además lee `ack`; **solo el backend** publica `ack` y `command` | `config/mosquitto/acl`; se prueba con `make probar-broker` |
| Cliente backend | `paho-mqtt` 1.6.1, `client_id` `aura-backend`, sesión persistente; arranca con la API si `MQTT_ENABLED=true` | `app/backend/services/mqtt_service.py`, `lifespan` en `app/backend/main.py` |
| Cliente raíz de la mesh | `ArduinoMqttClient` 0.1.8, `client_id` = su `hw_id`, sesión persistente, usuario `raiz` (`MQTT_CLAVE` en su `config_local.h` = `MQTT_RAIZ_PASSWORD`) | `aura-firmware/infraestructura/nodo_raiz/nodo_raiz.ino` |
| Cliente bridge | `paho-mqtt` 1.6.1 | `servicios/lorawan_bridge/lorawan_bridge/__main__.py` |

⚠️ **Sin TLS.** Las claves y los mensajes viajan en claro: aceptable en la red de laboratorio,
**no** en el despliegue del campus. Con credenciales y ACL, nadie sin clave puede publicar, y un
adaptador no puede publicar `ack` ni `command` (§3.6); pero quien tenga la clave de un adaptador
puede inyectar mediciones de cualquier placa, porque la ACL no distingue `mac-` de `eui-` dentro
de un nivel del tópico. La API HTTP tampoco publica en `hw/…` (`POST /api/v1/mqtt/publish` lo
rechaza).

Del lado LoRaWAN la radio está cifrada: cada dispositivo tiene su propia clave AES (OTAA) y el
gateway no puede leer ni falsificar el contenido. En la mesh, la asociación entre nodos usa WPA2
con la clave de la mesh, y el IE de mesh va cifrado. La debilidad está de los adaptadores para acá.

### Last Will and Testament

Cada adaptador registra un LWT con su propio `hw_id` antes de conectar al broker AURA:

| Adaptador | Tópico | Payload | Retain / QoS |
|---|---|---|---|
| `lorawan-bridge` | `hw/svc-lorawan-bridge/status` | `{"status":"offline"}` | `true` / `1` |
| raíz de la mesh | `hw/mac-<MAC del raíz>/status` | `{"status":"offline"}` | `true` / `1` |

Un `offline` retenido de un adaptador significa que **ningún** dispositivo de ese adaptador está
llegando, aunque cada uno siga apareciendo `online` con su último estado retenido. El backend
debe leerlo así. Si hay varios raíces (uno por edificio), cada uno tiene su `hw_id` y su LWT.

El `offline` **de cada dispositivo** no puede venir de un LWT, porque el nodo no tiene conexión
MQTT propia: lo infiere su adaptador (§3.2).

---

## 2. Árbol de tópicos e identidad

### 2.1 Broker AURA — lo que ve el backend

**Regla de oro: el `hw_id` viaja en el tópico, no en el payload.** Un payload que traiga un
identificador adentro no es un error, pero ese campo **se ignora**.

```
hw/<hw_id>/data            ← adaptador publica   → backend consume
hw/<hw_id>/ack             ← backend publica     → raíz de la mesh consume
hw/<hw_id>/status          ← adaptador publica   → backend consume   [retain]
hw/<hw_id>/command         ← backend publica     → adaptador consume
hw/<hw_id>/response        ← adaptador publica   → backend consume
hw/<hw_id>/alerts/<tipo>   ← adaptador publica   → backend consume
```

QoS 1 en todos. **Formato del `hw_id`**, siempre en minúsculas:

```
^(mac-[0-9a-f]{12}|eui-[0-9a-f]{16}|svc-[a-z0-9-]{1,32})$
```

| Prefijo | Qué identifica | Ejemplo |
|---|---|---|
| `mac-` | una placa de la mesh, por su MAC de fábrica (interfaz STA) | `mac-e072a1f7efe4` |
| `eui-` | un nodo LoRaWAN, por su DevEUI | `eui-798a381dd8c1cba4` |
| `svc-` | un adaptador que no es una placa | `svc-lorawan-bridge` |

`<tipo>` de las alertas: `[a-z_]{1,24}`.

"Adaptador" es el `lorawan-bridge` o el raíz de la mesh, según por dónde llegue la placa. Cada
`hw_id` pertenece a **un solo** adaptador: el raíz solo consume `hw/mac-…/{ack,command}` y el
bridge solo `hw/eui-…/command`. Lo demás lo ignoran sin mandar nada a la radio.

**El backend recibe lo que él mismo publica** (`ack`, `command`) si se suscribe con un comodín
que lo incluya (`hw/#`). Tiene que ignorarlo sin `warning`: si no, cada `ack` propio aparece en el
log como tópico no reconocido.

**Sin prefijo `aura/`.** Los tópicos arrancan directamente en `hw/`. Esto contradice otros
documentos del repo; ver §8.

El backend se suscribe a `hw/+/data`, `hw/+/status`, `hw/+/response` y `hw/+/alerts/+`
(`TOPICOS_BACKEND` en `app/backend/services/mqtt_service.py`) y parsea el tópico con
`parsear_topico` (`app/backend/ingesta/hw.py`). Un tópico fuera del árbol `hw/`, o con un `hw_id`
que no cumple el formato, se descarta con `logger.warning`.

### 2.2 Broker LoRaWAN — lo que consume el bridge

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

| Concepto de ChirpStack | Qué es en AURA |
|---|---|
| Tenant | uno: la UNRaf |
| Application | una: `aura` |
| Device profile | un **tipo** de nodo: codec, versión LoRaWAN, clase A/C, límite de downlink (§3.3) |
| Device | cada placa; su DevEUI es su `hw_id` (`eui-…`) |

El bridge traduce sin tabla: `hw_id = "eui-" + devEUI` en minúsculas, y para la bajada, al revés.
No necesita el tag `aura_device_id` de la v2.0/v3.0.

### 2.3 Identidad por placa ([ADR-005](adr/ADR-005-identidad-por-placa-y-mapeo-en-aura.md))

Los adaptadores **no conocen el UUID** de AURA. El mapeo vive en la plataforma:

- **`devices.hw_id`**: la placa asignada a cada dispositivo. **Una placa por dispositivo a la
  vez.** Asignar otra placa a un dispositivo que ya tenía una es el **reemplazo de placa**: la
  vieja queda desasignada y el dispositivo conserva su UUID y su historial.
- **Cuarentena**: un `data` de un `hw_id` que ningún dispositivo tiene asignado se guarda en
  `ts_telemetry` con `device_id` y `tenant_id` nulos, y se confirma con `ack`
  `resultado: "cuarentena"` (la hoja libera su cola). El mensaje completo va al log. La
  cuarentena se conserva 30 días.
- **Asignar** una placa reclama su cuarentena desde una fecha opcional: esas filas pasan a
  ser del dispositivo.
- **Placas ignoradas** (`hw_ignorado`: de banco, de pruebas): se confirman con
  `descartado` y no se guardan.

**Dar de alta un dispositivo** es: leer el `hw_id` de la placa (`aura-firmware/herramientas/leer_mac`
o el DevEUI) y asignarlo al dispositivo en AURA. No se toca el firmware de ningún adaptador.

**En la mesh**, el `hw_id` es la MAC de fábrica de la hoja, que el raíz recibe como origen de
cada paquete (`esp_mesh_recv`). Los nodos encuentran al raíz por el **ID de la mesh**, no por su
MAC: reemplazar el raíz no obliga a reflashear ningún nodo.

**Aula y concentrador (v4.1, [ADR-007](adr/ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md)).**
Cada aula tiene un **concentrador**, un nodo de la mesh de la cátedra que es el padre de las hojas
de esa aula: ve sus datos y los reenvía al raíz una sola vez, juntos (trama interna LOTE, cada
muestra con la MAC de su hoja). El raíz publica cada muestra con el `hw_id` de **la hoja que la
midió**, no con el del concentrador: para AURA el concentrador es transparente, como un relevo.
El aula **no está en el firmware**: AURA se la asigna a la hoja y al concentrador con el parámetro
reservado `aula` de `set_config` (§3.3), y la placa la guarda en memoria no volátil. Una placa
recién grabada no tiene aula: se cuelga del raíz o de cualquier concentrador, manda directo y AURA
la ve en cuarentena hasta que la asigna. Todas las hojas de un mismo tipo llevan el mismo binario.

### 2.4 Un dispositivo es una placa

En los dos transportes, **un dispositivo es una placa** (a la vez), no una sonda. Un nodo con
varias sondas publica con un solo `hw_id` y distingue cada sonda por el nombre del campo en
`values` (§3.1). Los parámetros que valen para todo el nodo (intervalo de envío) se configuran
sobre ese único dispositivo.

Se descartó un dispositivo por sonda porque los parámetros del nodo quedarían repetidos en varios
dispositivos de AURA. Cambiarlos desde uno modificaría los otros sin que AURA lo registre.

---

## 3. Mensajes

### 3.1 `hw/<hw_id>/data` — telemetría

**Emisor**: el adaptador de la placa · **QoS 1** · **retain `false`**.
- `lorawan-bridge`: uno por cada `event/up` con payload decodificado.
- raíz de la mesh: uno por cada TELEMETRIA de una hoja, incluidos los reintentos (llevan el
  mismo `ingest_id`).

```json
{ "values": { "temp_heladera_c": 4.5, "temp_freezer_c": -18.0 },
  "ingest_id": "5f0c1b1e-8a6d-4a55-9f2b-7c3e2d1a0b99",
  "ts": "2026-10-05T14:03:00Z",
  "adaptador": "mac-e072a1d848b0" }
```

| Campo | Obligatorio | Tipo | Notas |
|---|---|---|---|
| `values` | **sí** | objeto | Las mediciones. En LoRaWAN, el campo `object` del uplink tal como lo devolvió el codec del device profile; en la mesh, el JSON de la TELEMETRIA de la hoja. |
| `ingest_id` | **sí para la mesh**; no para LoRaWAN | UUID | Mesh: generado en la hoja y guardado con la muestra (§5). LoRaWAN: el `deduplicationId` de ChirpStack. Sin `ingest_id` el backend guarda pero **no publica `ack`** (§3.6). |
| `ts` | no | ISO 8601 UTC | Hora de la medición (ver abajo). |
| `adaptador` | no, pero todo adaptador lo manda | `hw_id` | El `hw_id` del adaptador que publica. Sirve para saber por dónde llegó una placa en cuarentena. |
| `unit` | no | objeto | El backend v3.0 lo lee (`_process_device_data`); ningún emisor lo manda. Default `{}`. |
| `quality` | no | entero | Leído, nadie lo manda. Default `100`. |

**No van `tenant_id`, `device_id` ni `type`.** El backend resuelve el dispositivo y su tenant
con `devices.hw_id` (§2.3). El adaptador no los conoce.

**`values` es opaco para el adaptador.** Su forma la define el codec de cada device profile en
LoRaWAN, y el firmware de la hoja en la mesh. Un codec nuevo no requiere cambiar el bridge.
**Si `values` no es un objeto JSON**, el raíz lo publica igual, como texto: el backend lo
rechaza con un `ack` `rechazado` y la hoja libera la muestra. Descartarlo en el raíz la dejaría
trabada para siempre en la cola de la hoja.

**Solo mediciones.** En `values` no van RSSI, SNR, contadores de trama ni nada que describa el
enlace o el estado del nodo: eso va a `status` (§3.2). Mezclarlos fue el problema de `pend` y
`desc` en la v1.x: un panel que graficaba "todas las variables del dispositivo" graficaba el
buffer de la mesh como si fuera un sensor.

Un uplink **sin** `object` (el device profile no tiene codec, o el codec falló) **no se
publica en `data`**. El bridge publica un `status` y lo registra con `logger.warning`: publicar
los bytes crudos en base64 dentro de `values` metería basura en la serie temporal.

**Varias sondas, un mensaje.** Un nodo con varias sondas (§2.4) las distingue por el nombre del
campo: `{"temp_heladera_c": 4.5, "temp_freezer_c": -18.0}`. Si las sondas tienen intervalos
distintos, cada mensaje lleva solo las que se midieron: `values` no tiene que traer todas.

**Una sonda que falla no se publica como medición.** Su campo **no aparece** en `values`: ni
un valor centinela (`327.67`, `-127`, `85`), ni `null`, ni la temperatura interna del
microcontrolador. La falla se informa por `hw/<hw_id>/alerts/sensor` (§3.5). Si ninguna sonda
del mensaje dio un valor válido, no se publica `data`.

**Hora de la medición: `ts`.** Campo opcional, ISO 8601 en UTC (`"2026-10-05T14:03:00Z"`). Para
LoRaWAN es el `time` del uplink en ChirpStack. Para la mesh es la hora de la hoja, que la toma de
la CONFIRMACION del raíz. Si el nodo no tiene hora válida, **se omite** y vale la hora de
llegada: no se inventa.

El backend lee `ts` (ISO 8601 con zona horaria; cualquier otra cosa vale como hora de llegada)
e `ingest_id`, y deduplica por `ingest_id` (`procesar_data` en `app/backend/ingesta/procesar.py`).

### 3.2 `hw/<hw_id>/status` — estado de la placa

**Emisor**: adaptador · **QoS 1** · **retain `true`** ← distinto de `data`

Todo lo que no es `status` va **anidado bajo `details`**:

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
| `status` | **sí** | Valores: `"online"`, `"offline"`. |
| `details` | no | Lo que el backend guarda del estado. |

Qué pone el bridge en `details` según el evento de ChirpStack (`traducir` en `servicios/lorawan_bridge/lorawan_bridge/traduccion.py`):

| Evento | `details` |
|---|---|
| `event/up` | `evento: "up"`, `rssi` y `snr` del mejor gateway, `gateway_eui`, `fcnt`, `dr`, `f_port` |
| `event/join` | `evento: "join"` |
| `event/status` | `evento: "status"`, `bateria` y `margen` tal como los informa ChirpStack |

Qué pone el raíz en `details` de una **hoja**. Lo publica cuando la hoja manda su reporte (al
unirse a la mesh, al volver después de perderla y después de aplicar una configuración):

| Campo | Notas |
|---|---|
| `evento` | `"reporte"` |
| `transporte` | `"mesh"` (el bridge pone `"lorawan"`) |
| `config` | la configuración **vigente** en la hoja, tal como la tiene guardada (§3.4) |
| `pendientes`, `descartadas` | muestras en la cola de la hoja y muestras perdidas por cola llena |
| `alimentacion` | `"red"`, `"bateria"` o `"desconocida"` |
| `modo` | `"sin_aula"` (todavía sin aula asignada), `"aula"` (colgada de su concentrador) o `"directo"` (no ve a su concentrador y manda directo al raíz) — v4.1 |
| `en_flash` | muestras de la cola respaldadas en flash (§5); solo si hay alguna — v4.1 |
| `eventos_perdidos` | resultados, reportes o alertas que no entraron en la cola de eventos; solo si hay alguno — v4.1 |
| `rssi`, `capa` | cómo recibe la hoja a su padre y en qué capa de la mesh está — v4.1 |
| `firmware` | `{"tipo": "aura-acceso", "version": "1.3.0"}`: el firmware de la placa (§3.3, actualización por la malla) — v4.3 |

En `config` va siempre `aula` (`null` si no tiene).

Qué pone el raíz en **su propio** `status`, cada 60 s: `rol: "raiz"`, `transporte: "mesh"`, `mac`,
`canal`, `nodos_en_mesh`, contadores (`publicadas`, `confirmadas`, `acks_ignorados`,
`values_invalidos`, `repetidas`, `ajenos`, `sin_lugar`, `version_distinta`, `invalidas`,
`desbordes_radio`), `hora_valida`, `uptime_s` y `firmware` (v4.3). `values_invalidos` cuenta las
TELEMETRIA cuyo `values` no era un objeto (§3.1). Mientras actualiza placas (§3.3) agrega
`ota: {"en_cola", "esperando_vuelta", "actual", "progreso", "imagen"}` y publica su `status` en cada
cuarto del avance.

El **concentrador** publica su propio `status` como cualquier placa de la mesh, con
`rol: "concentrador"`, su `config` (incluida `aula`) y su `firmware`. Sin aula no anuncia nada a las
hojas.

El contador de la cola y la alimentación van acá y **no** en `values`, por la misma razón que
`pend` y `desc` en la v1.x.

**`offline` por inferencia.** Como no hay LWT por placa, el adaptador publica
`{"status":"offline","details":{"motivo":"sin_tramas"}}` (retain) cuando una placa pasa
`3 ×` su intervalo sin transmitir. En LoRaWAN el intervalo sale del campo *uplink interval*
del device profile, y el último uplink, del `last_seen_at` que lleva ChirpStack: el bridge los lee
por la API cada `CS_REVISION_OFFLINE_S` (60 s), así que la inferencia sobrevive a sus reinicios.
Agrega a `details` `transporte`, `ultimo_uplink` e `intervalo_s`. **El *uplink interval* del
profile tiene que ser el real** (ChirpStack trae 3600 s por defecto: con eso, un nodo que manda
cada 5 min recién sale `offline` a las 3 h). Sin `CS_API_TOKEN`, el bridge no lo infiere. En la mesh sale de la configuración vigente que reportó la hoja
(`config.intervalo_s`); sin reporte no se infiere nada. El siguiente mensaje la vuelve a `online`.

**`offline` por salida de la mesh (v4.2).** El raíz además publica
`{"status":"offline","details":{"motivo":"fuera_de_la_malla"}}` (retain) cuando una placa falta de
la tabla de ruteo de la mesh más de 20 s, sin esperar los 3 intervalos. El raíz recuerda las placas
que vio (en su memoria no volátil), así que tras reiniciarse también publica `offline` de las que no
vuelven. El backend tiene que aceptar los dos motivos (`sin_tramas` y `fuera_de_la_malla`); para
AURA significan lo mismo: la placa no está.

`retain: true` es deliberado: un backend que arranca después recibe el último estado conocido
de cada placa de inmediato, sin esperar el próximo uplink, que en LoRaWAN pueden ser minutos.

### 3.3 `hw/<hw_id>/command` — comando hacia la placa

**Emisor**: backend · **Consumidor**: el adaptador de la placa · **QoS 1**

```json
{ "command": "led", "params": { "encendido": true }, "command_id": "c-2026-0001" }
```

| Campo | Obligatorio | Notas |
|---|---|---|
| `command` | **sí** | |
| `params` | no | Default `{}` |
| `command_id` | no | Si viene, el adaptador lo devuelve en cada `response` de ese comando (§3.4). Sin él, las respuestas no se pueden correlacionar con el comando. |

**Configuración remota.** Toda placa que tenga parámetros (intervalo, umbrales locales,
calibración) los recibe con el comando `set_config`:

```json
{ "command": "set_config", "params": { "intervalo_s": 300 }, "command_id": "c-2026-0002" }
```

`params` lleva **solo los parámetros que cambian**. El nodo valida cada valor contra su rango
antes de aplicarlo, lo guarda en memoria no volátil y, si algo no valida, **no aplica nada** del
comando. Un comando mal armado no puede dejar el nodo inutilizable.

**Parámetro reservado `aula` (mesh, v4.1).** En la mesh, `set_config` acepta `aula` en toda hoja
y todo concentrador, además de los parámetros propios del dispositivo: `{"aula": "aula1"}`. Es un
nombre corto en minúsculas (`a-z`, `0-9`, `-`, `_`), de 1 a 15 caracteres. Lo maneja la biblioteca
de la mesh, no el código del grupo. Motivos de rechazo propios: `aula_invalida` (no cumple el
formato) y `sin_parametros` (vino otro parámetro y el dispositivo no tiene ninguno). Un
`set_config` que trae `aula` y otros parámetros sigue la regla de todo o nada.

**Camino del comando en LoRaWAN.** El bridge saca el DevEUI del `hw_id` y encola un downlink
**confirmado** con la **API gRPC de ChirpStack** (`DeviceService.Enqueue`, con el API key de
`CS_API_TOKEN`), en el puerto `CS_FPORT_COMANDOS` (10 por defecto). Al `encodeDownlink` del
device profile le llega como `object`:

```javascript
{ "command": "set_config", "params": { "intervalo_s": 300 }, "ref": 7 }
```

`ref` es un número de 1 a 255, por placa, que pone el bridge: el `command_id` es texto libre y no
entra en los bytes de un downlink, así que el nodo devuelve el `ref` en su reporte (§3.4) y el
bridge lo traduce. Da la vuelta después del 255: un comando con más de 255 posteriores en vuelo
ya no se puede confirmar. Se usa la API y no el tópico `…/command/down` porque la API devuelve el
error en la llamada: con el tópico, un codec que no puede codificar el comando solo deja un
warning en el log de ChirpStack, y el comando quedaría sin respuesta. El `id` del ítem lo asigna
ChirpStack (la API ignora el que se le pasa) y es lo que ata el `txack` y el `ack` al comando.

**Límite de tamaño en LoRaWAN.** El límite real es en **bytes en el aire después del codec**
y depende del *data rate*. Lo valida el bridge contra el tag `aura_max_downlink_bytes` del
**device profile**. Sin ese tag, el bridge **no encola** y publica `rechazado`. El tamaño después
del codec solo se conoce con el ítem en la cola: si no entra, el bridge **vacía la cola de esa
placa** (ChirpStack no borra un ítem suelto), rechaza ese comando y también los otros que
estaban en ella. Motivos de `rechazado` del bridge:

| `motivo` | Cuándo |
|---|---|
| `json_invalido` | el payload no es JSON o no trae `command` |
| `bajada_no_configurada` | el bridge corre sin `CS_API_TOKEN` |
| `sin_limite_de_downlink` | el device profile no tiene el tag `aura_max_downlink_bytes` |
| `comando_muy_grande` | después del codec no entra en el límite (`details.bytes`, `details.limite`) |
| `cola_vaciada` | estaba en la cola de una placa que se vació por un `comando_muy_grande` |
| `no_existe_en_chirpstack` | el DevEUI no es un device de ChirpStack |
| `error_de_codec` | el `encodeDownlink` falló (`details.detalle` trae el error de JavaScript) |
| `chirpstack_no_disponible`, `chirpstack_sin_permiso`, `error_de_chirpstack` | errores de la API |
| `no_acusado` | `event/ack` con `acknowledged: false` |

**La correlación vive en memoria.** Si el bridge se reinicia con comandos en vuelo, sus `txack`,
`ack` y reportes se ignoran con un `warning`, y el comando queda en su último estado. Es una
debilidad aceptada: los comandos LoRaWAN son pocos y se pueden reenviar.

**Camino del comando en la mesh.** El raíz manda el JSON tal cual, como COMANDO, con
`esp_mesh_send()` a la MAC del `hw_id`; la mesh lo enruta por los relevos que hagan falta. El
límite es el **payload de la trama, 180 B**. El raíz publica `rechazado` con `motivo`:

| `motivo` | Cuándo |
|---|---|
| `json_invalido` | el payload no es JSON o no trae `command` |
| `comando_muy_grande` | el JSON no entra en 180 B (no se trunca: llegaría otro comando) |
| `nodo_no_alcanzable` | la mesh no acepta el envío: la hoja no está en la mesh |
| `el_raiz_no_acepta_comandos` | el `hw_id` es el del propio raíz y el comando no es `ota` |

El raíz **no** valida qué comandos acepta cada hoja: la hoja rechaza los que no conoce
(`comando_desconocido`). **Ningún comando se descarta sin respuesta.**

**Actualización por la malla (mesh, v4.3,
[ADR-008](adr/ADR-008-actualizacion-por-la-malla-y-tipo-de-firmware.md)).** El comando `ota` cambia el
firmware de una placa de la mesh —hoja, concentrador o el propio raíz— sin USB:

```json
{ "command": "ota", "command_id": "c-2026-0042",
  "params": { "url": "https://aura.example/firmware/aura-acceso-1.3.0.bin",
              "sha256": "<64 hex del .bin>" } }
```

| Campo | Obligatorio | Notas |
|---|---|---|
| `params.url` | **sí** | **`https://`** (v4.4), hasta 159 caracteres. El raíz la descarga **una vez** aunque se la pidan varias placas. `http://` solo lo acepta un raíz compilado para el banco. |
| `params.sha256` | **sí** | del `.bin` completo: el raíz no instala nada que no coincida. |

**Imágenes firmadas (v4.4).** Cada placa instala por OTA solo una imagen **firmada con la clave de la
cátedra** (RSA-3072, esquema de Secure Boot V2, sin eFuses): la firma se verifica en la placa antes
de dejarla para el próximo arranque, y una imagen sin firma o firmada con otra clave se rechaza con
`imagen_invalida`. La placa acepta las tramas de la actualización solo del raíz. `forzar_tipo` ya no
existe: cambiarle el uso a una placa se hace por USB (`forzar_tipo_deshabilitado`).

Lo atiende el raíz y **no** se reenvía como COMANDO, así que el límite de 180 B no aplica. El
**tipo** del firmware es el nombre del proyecto y la **versión**, su `PROJECT_VER`: los dos están
dentro de la imagen y cada placa los informa en `status.details.firmware` (§3.2). Regla del tipo:
`aura-<uso>` con `a-z`, `0-9` y `-`, hasta 31 caracteres; **todas las placas de un mismo uso llevan
el mismo tipo**. **"Todas las de un tipo"** es AURA mandando el comando a cada `hw_id` cuyo
`details.firmware.tipo` sea el de la imagen (y, si se quiere, cuya `config.aula` sea una dada).

El raíz atiende **una placa por vez** (las demás esperan en su cola, hasta 32) y deja **la suya para
el final**. Motivos de `rechazado` propios de `ota`:

| `motivo` | Cuándo |
|---|---|
| `url_invalida`, `sha256_invalido`, `params_invalidos` | el comando está mal armado (o la URL no es `https://`) |
| `forzar_tipo_deshabilitado` | vino `forzar_tipo` (v4.4: ya no existe) |
| `descarga_fallida`, `imagen_muy_grande` | no se pudo bajar la imagen (servidor caído, HTTP ≠ 200, corte) o no entra |
| `sha256_distinto` | lo descargado, o lo que recibió el nodo, no coincide con `params.sha256` |
| `imagen_invalida`, `imagen_sin_tipo_o_version` | no es una imagen de ESP-IDF válida, **no tiene la firma de la cátedra** (v4.4), o su tipo o versión no cumplen la regla |
| `tipo_distinto` | la placa es de otro tipo (`details.firmware` dice qué tiene) |
| `chip_distinto` | la imagen es para otro chip (por ejemplo, de ESP32-S3 para una ESP32-C3) |
| `nodo_no_alcanzable`, `nodo_no_responde` | la placa no está en la mesh, o no contesta (por ejemplo, un firmware anterior a la v4.3, que no sabe actualizarse) |
| `transferencia_cortada` | la placa dejó de pedir bloques: se reintenta desde cero dos veces antes de rechazar |
| `version_actual_sin_confirmar` | la placa todavía está probando una versión recién instalada: queda confirmada cuando habla con el raíz y anda 60 s, y si no lo logra vuelve a la anterior a los 10 min |
| `volvio_a_la_version_anterior` | la versión nueva se instaló pero no anduvo, y la placa volvió sola a la anterior (`details.firmware` dice cuál quedó) |
| `no_volvio` | la placa no volvió a la mesh en 15 min después de reiniciarse |
| `actualizacion_en_curso`, `reemplazado_por_otro_pedido`, `cola_ota_llena` | ya se la está actualizando; un pedido nuevo para la misma placa reemplaza al que esperaba; más de 32 en espera |

Si el raíz se reinicia, **olvida su cola**: quien pidió la actualización vuelve a mandar lo que no
terminó. Es idempotente: una placa que ya tiene esa versión contesta `aplicado` con
`details.sin_cambios: true`.

**Latencia.** En la mesh la hoja escucha todo el tiempo: el comando llega en menos de un
segundo. En LoRaWAN, un dispositivo **clase A** solo abre la ventana de recepción después de
transmitir: el comando sale con su próximo uplink, que puede tardar minutos. Los dispositivos
que tienen que ejecutar un comando en segundos (actuadores de iluminación, cerraduras) van en
**clase C**, que escucha todo el tiempo y es viable porque están alimentados de red.

### 3.4 `hw/<hw_id>/response` — confirmación del comando

**Emisor**: adaptador · **QoS 1** · **retain `false`**

```json
{ "status": "aplicado", "details": { "command_id": "c-2026-0001", "config": { "intervalo_s": 60 } } }
```

| `status` | Qué significa | LoRaWAN | Mesh |
|---|---|---|---|
| `encolado` | el comando quedó en una cola esperando salir | `Enqueue` de la API aceptó el comando y entra en el límite | no se usa: la mesh no encola |
| `transmitido` | salió por radio | `event/txack` | `esp_mesh_send` aceptó el comando |
| `recibido` | el nodo acusó recibo | `event/ack` con `acknowledged: true` (solo downlinks confirmados) | la hoja devolvió el resultado del comando |
| `aplicado` | **el nodo informó que lo ejecutó** | uplink de reporte del nodo con `aplicado: true` (ver abajo) | resultado de la hoja con `aplicado: true` |
| `rechazado` | no salió, el nodo no lo acusó, o el nodo no lo aplicó | error de la API, tamaño excedido (§3.3), `acknowledged: false`, o reporte del nodo con rechazo | los motivos de §3.3, resultado de la hoja con rechazo, o `sin_resultado_del_nodo` (30 s sin resultado) |

El motivo de un `rechazado` va en `details.motivo`.

**En `ota` (mesh, v4.3)** los estados significan: `transmitido` = el raíz le ofreció la imagen a la
placa; `recibido` = la placa la recibió entera, la verificó y se reinicia con ella; `aplicado` = la
placa **volvió con la versión nueva y anduvo 60 s sin colgarse** (`details.firmware`, y
`details.sin_cambios` si ya la tenía); `rechazado` = los motivos de §3.3. Entre `recibido` y
`aplicado` pasan ~1 min; una placa que no anda vuelve sola a su versión anterior.

**`aplicado` es el único estado que confirma ejecución.** Lo publica el adaptador **solo** cuando
el firmware lo informa explícitamente (en la mesh con el `command_id`; en LoRaWAN con el `ref`). Junto con `aplicado`, `details.config`
trae la configuración que quedó vigente en el nodo, para que el backend la compare con lo que
pidió. Los otros estados no confirman ejecución: `recibido` solo dice que el comando llegó.

En LoRaWAN, el nodo informa la ejecución en un uplink (el puerto lo decide el codec), que el
`decodeUplink` traduce a:

```javascript
{ "ref": 7, "aplicado": true, "config": { "intervalo_s": 300 } }
```

`motivo` es opcional y va con `aplicado: false` (si falta, el bridge pone
`rechazado_por_el_nodo`). Un `object` con `ref` entero y `aplicado` booleano **es un reporte y
no se publica en `data`**: un codec de medición no puede usar esos dos nombres de campo. Como el nodo
clase A solo puede transmitir en su turno, `aplicado` llega con algún uplink posterior: hasta
entonces, la interfaz muestra el cambio como **pendiente**.

El backend acepta los cinco estados y actualiza el comando por `details.command_id`
(`procesar_response` en `app/backend/ingesta/eventos.py`, tabla `comandos`): `aplicado` marca
`confirma_ejecucion` y guarda `details.config`; `rechazado` guarda `details.motivo`. Un estado
final (`aplicado`, `rechazado`) no se pisa con uno intermedio que llegue tarde.

### 3.5 `hw/<hw_id>/alerts/<tipo>`

**Emisor**: adaptador · **QoS 1** · **retain `false`**

```json
{ "severity": "high", "message": "Sonda freezer sin respuesta",
  "details": { "campo": "temp_freezer_c", "motivo": "sin_respuesta" },
  "ts": "2026-10-05T14:03:00Z" }
```

| Campo | Obligatorio | Notas |
|---|---|---|
| `severity` | no | `info`, `warning`, `high`, `critical`. Default `unknown` |
| `message` | no | texto para una persona |
| `details` | no | objeto; depende del tipo |
| `ts` | no | como en §3.1 |

Tipos especificados. El tipo va en el tópico (`[a-z_]{1,24}`; el raíz no publica otro):

| Tipo | Cuándo | `details` |
|---|---|---|
| `sensor` | una sonda dejó de responder o da un valor fuera de su rango físico | `campo` (el nombre que tendría en `values`) y `motivo`: `sin_respuesta`, `fuera_de_rango` |
| `energia` | el nodo pasó a batería o volvió a la red | `alimentacion`: `"bateria"` o `"red"` |

La alerta se publica **una vez por cambio**, no en cada medición: una sonda que falla durante una
hora es una alerta, no sesenta. Cuando la sonda vuelve, se publica otra con `severity: "info"` y
`motivo: "recuperada"`.

Las alertas por **umbral** (temperatura fuera del rango de conservación) **no** las emite el
adaptador: las calcula AURA a partir de `values`. Un LED de alarma local en el nodo es
independiente de eso.

La alerta `dispositivo_desconocido` de la v3.1 no existe en la v4.0: una placa desconocida va a
cuarentena (§2.3).

### 3.6 `hw/<hw_id>/ack` — confirmación de la muestra

**Emisor**: backend · **Consumidor**: raíz de la mesh · **QoS 1** · **retain `false`**

El backend lo publica por cada `data` que trae `ingest_id`, **después del commit** en la base. Es
la única señal de que una muestra quedó resuelta
([ADR-004](adr/ADR-004-mqtt-punto-comun-de-ingesta-con-ack-de-persistencia.md)).

```json
{ "ingest_id": "5f0c1b1e-8a6d-4a55-9f2b-7c3e2d1a0b99", "resultado": "persistido" }
```

| Campo | Obligatorio | Notas |
|---|---|---|
| `ingest_id` | **sí** | el de la muestra que se confirma, en el formato con guiones |
| `resultado` | **sí** | uno de los cinco de la tabla |
| `motivo` | solo con `rechazado` | por ejemplo `values_invalido` |

| `resultado` | Qué significa | Qué hace el raíz |
|---|---|---|
| `persistido` | la muestra se guardó con su dispositivo | CONFIRMACION a la hoja |
| `duplicado` | ese `ingest_id` ya estaba en la base: es un reintento | CONFIRMACION a la hoja |
| `cuarentena` | la placa no está asignada: se guardó sin dispositivo (§2.3) | CONFIRMACION a la hoja |
| `descartado` | la placa está ignorada: no se guardó (§2.3) | CONFIRMACION a la hoja |
| `rechazado` | la muestra **nunca** va a poder guardarse (`values` no es un objeto, campo con tipo inválido) | CONFIRMACION a la hoja, para no trabar su cola |
| *(sin ack)* | error transitorio (la base no responde) o el mensaje no llegó | nada: la hoja reintenta con su espera (15 s … 5 min) |

**Validación fallida con `ingest_id`: `rechazado`.** Un `data` que no pasa la validación del
backend, pero trae `ingest_id`, se contesta con `rechazado` y su `motivo`. Descartarlo sin `ack`
dejaría la muestra reintentándose para siempre en la hoja.

**Errores transitorios: sin ack.** Si la base no responde, el backend no publica nada. La hoja
reintenta y, cuando la base vuelve, el reintento se guarda o sale como `duplicado`. Publicar
`rechazado` ante un error transitorio borraría una muestra válida.

**Un `ack` que el raíz no entiende no confirma.** Un `resultado` fuera de los cinco, o un
`ingest_id` mal formado, se ignora y se cuenta en `acks_ignorados`: ante la duda la hoja
reintenta, y AURA contestará `duplicado`. Confirmar de más borraría una muestra.

**Quién publica acá.** Solo el backend. Quien pueda publicar en `hw/+/ack` puede hacer que una
hoja borre muestras que no se guardaron: la ACL del broker (§10, pendiente 11) tiene que
reservar este tópico.

**El `lorawan-bridge` no se suscribe.** El nodo LoRaWAN no guarda muestras y ChirpStack no
retransmite, así que no hay nada que confirmar. El `ack` sirve igual para medir, comparando
uplinks contra acks, cuánto se perdió.

---

## 4. Handlers del despacho

Implementación en `app/backend/ingesta/`, llamada desde el thread de paho (síncrona, sin event
loop):

| Función | Tópico | Qué hace |
|---|---|---|
| `despachar` | `hw/<hw_id>/…` | parsea el tópico y el `hw_id`; ignora sin `warning` lo que publica el propio backend (`ack`, `command`); ante un error de base hace rollback y **no** publica `ack` |
| `procesar_data` | `hw/<hw_id>/data` | busca `devices.hw_id`; guarda con dispositivo, en cuarentena o descarta (ignorada); deduplica por `ingest_id`; publica el `ack` después del commit |
| `procesar_status` | `hw/<hw_id>/status` | actualiza `connection_status` y `last_seen` del dispositivo; `details` va al log |
| `procesar_response` | `hw/<hw_id>/response` | actualiza el comando en la tabla `comandos` (§3.4) |
| `procesar_alerta` | `hw/<hw_id>/alerts/<tipo>` | guarda la alerta en la tabla `alertas` (§3.5) |

Placas y comandos por API (con sesión): `GET /api/v1/hw/descubiertas`,
`POST /api/v1/hw/{hw_id}/asignar` e `/ignorar`, `POST /api/v1/devices/{id}/comandos` (publica en
`hw/<placa>/command` y registra el comando), `GET /api/v1/devices/{id}/comandos` y `/alertas`.
La cuarentena vieja se purga con `app/scripts/purgar_cuarentena.py`.

Además, todo payload que no sea un objeto JSON se descarta con `warning`, antes de despachar.

---

## 5. Entrega, duplicados e idempotencia

**Todo el sistema usa QoS 1: "al menos una vez".** El broker puede entregar el mismo mensaje
más de una vez, así que todo consumidor debe ser idempotente.

| Capa | Mecanismo | Alcance |
|---|---|---|
| Radio → ChirpStack | ChirpStack junta las recepciones del mismo uplink por varios gateways y emite **un** evento con un `deduplicationId` | varios gateways, un solo dato |
| Nodo → ChirpStack | uplinks confirmados con reintentos (firmware de `IC-lorawan-test`) | ⚠️ cómo trata ChirpStack un reintento con el mismo `fCnt` está **sin verificar en banco** |
| Bridge → AURA | `ingest_id` = `deduplicationId` en `data` | sobrevive a reinicios del bridge: el id lo genera ChirpStack |
| Hoja → raíz | la hoja **guarda cada muestra** en una cola en RAM (400 muestras) y la reenvía hasta recibir la CONFIRMACION; lo que pasa **5 min sin confirmar** se respalda en flash (v4.1) | sobrevive a cortes del raíz, del WiFi, del broker y de AURA (unos 66 min de corte midiendo cada 10 s); **un reinicio de la hoja pierde lo que tenía menos de 5 min sin confirmar** |
| Raíz → AURA | `ingest_id` generado **en la hoja**, un UUID por muestra, guardado con ella antes del primer envío | igual en cada reintento y después de reiniciar cualquier placa |
| Backend → raíz | `ack` con el `ingest_id`, después del commit (§3.6) | un `ack` perdido solo provoca un reintento, que vuelve como `duplicado` |
| Backend (REST) | `ingest_id` con `unique=True` en `ts_telemetry` (`models/telemetry.py`) | deduplicación real en base |
| Backend (MQTT) | `SELECT` explícito por `ingest_id` antes de insertar (`procesar_data`) | deduplicación real, aunque la muestra llegue sin `ts` |

**Deduplicar por `ingest_id`, no por el índice.** El índice único real de `ts_telemetry` es
`(tenant_id, ingest_id, ts)`. Una muestra de la mesh sin `ts` (la hoja todavía no tiene hora)
llega con otra hora de llegada en cada reintento y el índice no la detecta: la deduplicación
tiene que buscar el `ingest_id` explícitamente.

**Por qué la cola de la hoja es híbrida (v4.1).** Escribir cada muestra en flash frenaba el loop
y gasta la memoria; en marcha normal la confirmación llega en menos de un segundo y la flash no se
toca. Solo un corte de más de 5 min la usa. **Debilidad aceptada**: si la hoja se reinicia durante
un corte, se pierden las muestras de sus últimos 5 min sin confirmar.

**Confirmación en la mesh.** El raíz confirma una muestra a la hoja **solo** cuando recibe el
`ack` de ese `ingest_id` (§3.6). Hasta entonces la hoja la conserva. Que `esp_mesh_send` acepte la
trama, o el PUBACK del broker, **no** son confirmación.

### Mensajes durante una caída

MQTT no tiene colas: el broker entrega a quien esté suscripto **en ese momento**. Para que
guarde los mensajes QoS 1 mientras un consumidor está caído, el consumidor tiene que conectar
con un **`client_id` fijo y sesión persistente** (`clean_session=False`).

El backend conecta como `aura-backend` y el bridge como `aura-lorawan-bridge` (broker AURA, para
los comandos) y `aura-lorawan-bridge-cs` (broker de ChirpStack, para los uplinks), todos con
`clean_session=False`: un reinicio de cualquiera de los dos no pierde los mensajes QoS 1 que
lleguen mientras tanto. El raíz de la mesh conecta con su `hw_id` como `client_id` y también con
sesión persistente: los `ack` y `command` publicados mientras reconecta lo esperan en el broker.
Un comando que llega así puede salir tarde; si la hoja no está en la mesh, se rechaza con
`nodo_no_alcanzable`. Por eso **no puede haber dos procesos
con el mismo `client_id`**: el broker desconecta al anterior (los tests del backend arrancan con
`MQTT_ENABLED=false` por esto).

En LoRaWAN, una muestra que el backend no pudo guardar por un error transitorio **no se
reintenta**: ver el pendiente 18.

Si algún día hay más de una réplica del backend, la suscripción pasa a ser compartida
(`$share/aura-backend/hw/+/data`), para que cada mensaje lo procese una sola réplica.

---

## 6. Ingesta REST

> **No es un camino de dispositivos** desde la v3.1
> ([ADR-004](adr/ADR-004-mqtt-punto-comun-de-ingesta-con-ack-de-persistencia.md)). Queda para
> cargas manuales y herramientas. Lo que sigue describe el endpoint tal como está.

`POST /api/v1/telemetry/ingest` (`app/backend/api/endpoints/telemetry.py`) es el único camino
que hoy **persiste y deduplica** en base. **No valida** que el `device_id` exista en `devices`:
`ts_telemetry` está en otra base, sin clave foránea.
Pide sesión, como todo `/api/v1` salvo login, refresh y logout.

```json
{ "events": [ {
    "tenant_id": "…", "device_id": "<uuid del dispositivo>", "type": "temperatura",
    "ts": "2026-10-05T14:03:00Z", "ingest_id": "<uuid>",
    "payload": { "temp_heladera_c": 4.5, "temp_freezer_c": -18.0 } } ] }
```

- `payload` sigue las mismas reglas que `values` en §3.1: solo mediciones, y una sonda que
  falla no aparece.
- Nada de diagnóstico del nodo en `payload`: eso va a `status` (§3.2) y a `alerts` (§3.5).

**Respuesta.** El cuerpo es `{"inserted", "duplicates", "errors", "message"}`
(`TelemetryBatchResponse`); los tres primeros son **enteros**. El código depende del resultado:

| Código | Cuándo |
|---|---|
| `201` | `errors == 0`: todos los eventos quedaron guardados (insertados o duplicados) |
| `207` | se guardó una parte: mirar `errors` |
| `422` | no se guardó ninguno, o el batch no pasa la validación |

Quien use el endpoint tiene que considerar guardado un evento si y solo si:

```
HTTP 201  y  errors == 0  y  inserted + duplicates == cantidad de eventos enviados
```

Un duplicado cuenta como guardado: es la misma muestra, que ya estaba en la base. Cada evento se
inserta en su propio *savepoint*: uno que falla no descarta a los demás del batch (antes de
2026-10-09 los descartaba y los contaba igual en `inserted`, y respondía `201` aunque fallaran
todos).

La integración HTTP de ChirpStack, que podría apuntar a este endpoint, sigue descartada para
LoRaWAN: postea el formato de eventos de ChirpStack y no el de AURA
([ADR-002](adr/ADR-002-lorawan-reemplaza-a-la-mesh-espnow.md)).

---

## 7. Versionado y evolución

El mensaje MQTT **no lleva ninguna marca de versión**. `AURA_PROTO_VERSION` de la mesh
(`aura-firmware/comun/protocolo_aura.h`) versiona la **trama entre los nodos de la mesh**, no el
mensaje MQTT: la v3 es la de ESP-WIFI-MESH, y la v2 (ESP-NOW) se descarta y se cuenta en
`version_distinta`. AURA no la ve.

Reglas de evolución de este contrato:

1. **Agregar un campo opcional** al payload no rompe nada: no requiere subir versión.
2. **Renombrar o eliminar un campo, o cambiar su tipo, unidad o conjunto de valores
   válidos**, es incompatible: subir la versión mayor de este documento y coordinar el
   despliegue.
3. **Cambiar el árbol de tópicos** es lo más caro: los dispositivos ya instalados siguen
   publicando en el viejo. Si pasa, el backend debe consumir ambos durante la transición. La
   v4.0 cambió el árbol (`devices/` → `hw/`) cuando ningún adaptador publicaba en producción.
4. Todo cambio incompatible se anota en §9.

En los dos transportes la regla 3 es barata para los nodos, porque los tópicos los arma el
adaptador y no el firmware del nodo. Con LoRaWAN se cambian sin reflashear nada; con la mesh hay
que reflashear **solo el raíz**. Lo que sí queda grabado en los nodos LoRaWAN es el **formato de
sus bytes**, que interpreta el codec del device profile: un cambio de formato en el firmware
requiere un device profile nuevo, no editar el codec del existente.

**Pendiente**: agregar un campo `v` al payload MQTT para que el backend pueda distinguir
generaciones de emisores sin adivinar por la forma del JSON.

---

## 8. ⚠️ Documentos y código que contradicen este contrato

**Este documento es el normativo**; los demás están desactualizados y hay que corregirlos:

| Documento | Qué dice | Estado |
|---|---|---|
| **Este documento** | árbol `hw/`, identidad por placa, `ack` (v4.0) | propuesta |
| backend (`app/backend/ingesta/`) y `lorawan-bridge` (`servicios/lorawan_bridge/`) | árbol `hw/`, identidad por placa, `ack` | ✅ con tests y probados con los brokers del banco |
| repo público `aura-firmware` | mesh v4.1 en ESP-IDF (`Mesh_Red_Completa/`: raíz, concentrador por aula, hoja sobre `nodo_mesh`), `hw/<hw_id>/…`, `ack` | ✅ probada en placa de punta a punta (2026-10-20); falta una prueba larga. La versión Arduino (`comun/`, `infraestructura/`) queda obsoleta ([ADR-007](adr/ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md)) |
| `docs/adr/ADR-004-…` | "un `device_id` desconocido no se confirma" (punto 5) | ⏸ reemplazado en ese punto por [ADR-005](adr/ADR-005-identidad-por-placa-y-mapeo-en-aura.md) |
| `docs/adr/ADR-003-…` | "la telemetría de la mesh entra por REST"; mesh ESP-NOW | ⏸ reemplazado por [ADR-004](adr/ADR-004-mqtt-punto-comun-de-ingesta-con-ack-de-persistencia.md) y [ADR-006](adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md) en esos puntos |
| `docs/adr/ADR-002-…` | "la mesh queda congelada"; tag `aura_device_id` | ⏸ reemplazado por ADR-003 y ADR-005 en esos puntos |
| `docs/CONTRATOS_APIS.md` | resume la API HTTP de placas y comandos, y remite a este documento | ✅ |
| `docs/Arquitectura_IoT_AURA.md` §2.1 | árbol `hw/`, remite a este documento | ✅ |
| `CLAUDE.md` | remite a este documento | ✅ |
| `.cursorrules` | árbol `hw/` e identidad por placa, remite a este documento | ✅ |
| `recursos/circutor_reader/mqtt_messages.json` | `aura/devices/circutor_…/telemetry` | ❌ mock data con otro esquema |
| `firmware/` de este repo | mesh ESP-NOW v1.x | ✅ eliminado (2026-10-09): el firmware vive en `aura-firmware`; la mesh v1 queda en su historia y en `aura-firmware/docs/historico/` |
| avisos de `docs/superpowers/` sobre la mesh | "congelada" | ⏸ son planes con fecha: registran lo que se decidió entonces |

**El caso Circutor sigue abierto.** El medidor se integra por Modbus TCP en `<IP interna>:502`, su
lector vive en el repositorio propio `aura-circutor` (ADR-001 de `aura-app`, no publicado) y
todavía no publica en ningún lado. Con el `lorawan-bridge` y el raíz de la mesh, el patrón queda establecido: **un adaptador
por protocolo que publica en `hw/<hw_id>/…`**. Para el Circutor, el `hw_id` sería `svc-…` o la
MAC del medidor. Mientras no se decida, sus mensajes se descartan:
`aura/devices/circutor_<IP>/telemetry` no matchea el árbol de §2.1.

---

## 9. Historial de cambios

| Versión | Fecha | Cambio |
|---|---|---|
| 4.4 | 2026-10-26 | **Propuesta. Seguridad de la actualización por la malla** (revisión de seguridad de la red, etapa 1). **Incompatible solo para quien use `forzar_tipo`**, que se elimina (`forzar_tipo_deshabilitado`). `params.url` pasa a ser `https://`. Las imágenes van **firmadas por la cátedra** y la placa rechaza una sin firma (`imagen_invalida`). Pendiente 23 resuelto en parte (firma sin Secure Boot). |
| 4.3 | 2026-10-22 | **Propuesta. Actualización por la malla** ([ADR-008](adr/ADR-008-actualizacion-por-la-malla-y-tipo-de-firmware.md), `aura-firmware` #24). Compatible: nada cambia de forma. Nuevos: el comando **`ota`** para cualquier placa de la mesh, incluido el raíz, que solo acepta ese comando para sí mismo (§3.3), con sus motivos de `rechazado` y el significado de cada estado (§3.4); **`details.firmware`** (`tipo` y `versión`) en el `status` de hojas, concentradores y raíz, y `details.ota` en el del raíz mientras actualiza (§3.2). Regla del tipo de firmware: `aura-<uso>`, uno por uso. Pendientes 22 y 23. |
| 4.2 | 2026-10-21 | **Propuesta.** Nuevo motivo de `offline` en la mesh: `fuera_de_la_malla` (§3.2), además de `sin_tramas`. Compatible si el backend no valida el motivo. Sin otros cambios de formato: el modo isla del concentrador, la trama interna `ESTADO` y las hojas ausentes en la pantalla del aula son internos de la mesh (`aura-firmware/Mesh_Red_Completa/`). |
| 4.1 | 2026-10-20 | **Mesh en ESP-IDF con concentrador por aula** ([ADR-007](adr/ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md), `aura-firmware` #21). Compatible: el árbol de tópicos y los payloads no cambian. Nuevos: el **concentrador del aula** (§1, §2.3, transparente para AURA), el parámetro reservado **`aula`** de `set_config` con los motivos `aula_invalida` y `sin_parametros` (§3.3), y en el `status` de la hoja `modo`, `en_flash`, `eventos_perdidos`, `rssi`, `capa` y `config.aula` (§3.2). La cola de la hoja pasa a **híbrida**: RAM, y flash solo para lo que pasa 5 min sin confirmar (§5). En las hojas ya no hace falta el SSID del WiFi del edificio: solo el raíz se conecta al router. Pendiente 20. |
| 4.0 | 2026-10-14 | El raíz de la mesh conecta con sesión persistente (pendiente 7); §1 y el pendiente 11 decían que no mandaba usuario y clave, y lo hace desde `aura-firmware` #9. |
| 4.0 | 2026-10-14 | §10: pendientes 18 (LoRaWAN pierde la muestra si falla la base) y 19 (codecs con diagnóstico en `values`), encontrados al documentar el recorrido de los datos. |
| 4.0 | 2026-10-14 | **`offline` inferido en el `lorawan-bridge`** (§3.2), con `last_seen_at` de ChirpStack y el *uplink interval* del profile; `details` suma `transporte`, `ultimo_uplink` e `intervalo_s`. |
| 4.0 | 2026-10-14 | **Bajada de comandos en el `lorawan-bridge`** (§3.3, §3.4). El codec recibe un `ref` (1-255) y el reporte del nodo lo devuelve en lugar del `command_id`: `{ref, aplicado, config, motivo?}`. Nadie implementaba el reporte con `command_id`, así que no se sube la versión. Motivos de `rechazado` del bridge, vaciado de la cola por tamaño y correlación en memoria. El bridge conecta al broker AURA con sesión persistente. |
| 4.0 | 2026-10-09 | **Implementada en el backend y el `lorawan-bridge`** (sin cambios en el formato). Pendientes 1, 3, 6 y 9 resueltos, 5 y 7 en parte. Se borra `app/backend/services/device_integration.py` (despacho del árbol `devices/` de la v3.0). |
| 4.0 | 2026-10-07 | **Propuesta. Identidad por placa y ESP-WIFI-MESH** ([ADR-005](adr/ADR-005-identidad-por-placa-y-mapeo-en-aura.md), [ADR-006](adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md)). **Incompatible**: el árbol pasa de `devices/<uuid>/…` y `alerts/<uuid>/<tipo>` a `hw/<hw_id>/…`; los adaptadores publican con el `hw_id` de la placa y AURA hace el mapeo (`devices.hw_id`, cuarentena, asignar, ignorar). Fuera el tag `aura_device_id`, la tabla MAC → UUID y la MAC lógica. Absorbe la v3.1 (no publicada): MQTT único camino de entrada y `ack` después del commit, ahora con cinco resultados (`persistido`, `duplicado`, `cuarentena`, `descartado`, `rechazado`). `data` pierde `type` y `tenant_id`. Se elimina la alerta `dispositivo_desconocido`. La mesh de interior pasa de ESP-NOW a ESP-WIFI-MESH: el gateway es el **raíz**, la sala es un **relevo**, la trama interna pasa a la v3. Aclaraciones de la revisión del borrador v3.1: `rechazado` ante validación fallida con `ingest_id` (§3.6), `values_invalidos` en el `status` del raíz (§3.2), §6 en presente solo para quien use el REST. |
| 3.1 | 2026-10-05 | **No publicada; absorbida por la 4.0.** MQTT como único camino de entrada con `devices/<id>/ack` ([ADR-004](adr/ADR-004-mqtt-punto-comun-de-ingesta-con-ack-de-persistencia.md)); MAC lógica para el gateway y las salas de la mesh. |
| 3.0 | 2026-10-05 | **Propuesta. Dos transportes** ([ADR-003](adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md)): mesh ESP-NOW en interior y LoRaWAN en exterior, cada uno con su adaptador sobre el mismo árbol. **Incompatible**: nuevo estado `aplicado` en `response`, que confirma ejecución y el backend todavía rechaza; un `device_id` por placa y no por sonda; una sonda que falla no aparece en `values`. Nuevos: tabla MAC → UUID, telemetría de la mesh por REST con criterio de persistencia, campo `ts`, `set_config`, alertas `sensor` y `energia`, `details` de la mesh en `status`, LWT del gateway de la mesh. Documentados dos defectos del endpoint REST (§6). |
| 2.0 | 2026-09-25 | **Los dispositivos pasan a LoRaWAN** ([ADR-002](adr/ADR-002-lorawan-reemplaza-a-la-mesh-espnow.md)). El árbol de tópicos no cambia; cambia el emisor: el `lorawan-bridge` reemplaza al gateway de la mesh. **Incompatible**: `response` pasa a `encolado`/`transmitido`/`recibido`/`rechazado` (se rechaza `enviado_a_mesh`); el backend deja de limitar el comando a 180 B, porque el límite pasa al bridge por device profile. Nuevos: tópicos de ChirpStack, tag `aura_device_id`, `ingest_id` en `data`, `details` de radio en `status`, `command_id`. Quedan sin objeto los campos `pend`, `desc` y `lux_sim` y el modo REST del gateway. |
| 1.2 | 2026-09-25 | §8: `CLAUDE.md` y `.cursorrules` alineados con este contrato; el lector Circutor ya vive en `aura-circutor`. |
| 1.1 | 2026-09-06 | Implementados los cuatro handlers faltantes de §4, con validación de longitud de comando y de severidad de alerta, y guardia de payload no-dict. |
| 1.0 | 2026-09-06 | Primera redacción, relevada del código en `main` tras la consolidación de ramas. Documenta el estado tal cual está, incluidos los desajustes conocidos. |

---

## 10. Pendientes

Ordenados por impacto. Los números se conservan entre versiones porque otros documentos los
citan.

1. ✅ **Que la ingesta MQTT persista.** El servicio MQTT arranca con la API y despacha en forma
   síncrona (MVP de ingesta, `docs/superpowers/plans/2026-10-05-mvp-ingesta-hw.md`).
2. ✅ **Corregir los dos defectos del endpoint REST** (§6), con tests
   (`app/tests/endpoints/test_telemetry_ingest.py`).
3. ✅ **Aceptar `aplicado`** con `confirma_ejecucion: True` (§3.4, tabla `comandos`).
4. ✅ **Raíz de la mesh en la v4.0** (`aura-firmware/infraestructura/nodo_raiz/`): `hw/<hw_id>/…`,
   `ack`, sin tabla ni REST. **Sin probar en placa**: ver el pendiente 17.
5. **`lorawan-bridge`**: ✅ la subida (`data` y `status`, LWT, sesión persistente) y ✅ la bajada
   de comandos por la API de ChirpStack y ✅ el `offline` inferido (2026-10-14, §3.2 y §3.3).
   Falta un codec y un firmware que reciban comandos, para probar la bajada con un nodo real.
6. ✅ **El backend en el árbol `hw/`** (§2.1, §4): `devices.hw_id`, cuarentena, asignar e
   ignorar (§2.3); `ingest_id` y `ts`, deduplicación y `ack` (§3.1, §3.6, §5).
7. ✅ **Sesión persistente con `client_id` fijo** (§5): backend, `lorawan-bridge` y raíz de la
   mesh (2026-10-14, compilado y sin probar en placa).
8. **Cifrado y claves de la mesh.** ESP-WIFI-MESH autentica la asociación con WPA2 (clave de la
   mesh) y cifra el IE de mesh, pero el payload entre nodos no tiene cifrado de punta a punta, y
   la clave de la mesh es una por edificio, compartida con los grupos: rotarla es reflashear todas
   las placas.
9. ✅ **Persistir `command`, `response` y `alerts`** (§4, tablas `comandos` y `alertas`). `status.details`
   sigue yendo solo al log.
10. **Verificar en banco** qué hace ChirpStack con un uplink confirmado reintentado con el mismo
    `fCnt` (§5).
11. ✅ **Credenciales y ACL en el broker AURA** (§1, 2026-10-09; `make probar-broker`); el raíz manda
    su usuario y clave desde `aura-firmware` #9. Falta TLS para el despliegue.
12. ✅ **Corregir los documentos de §8** (2026-10-09). Sigue abierto el caso Circutor (§8).
13. ✅ **Test de contrato** (`app/tests/contract/test_contrato_mqtt.py`): lee los ejemplos JSON,
    el formato del `hw_id`, el árbol de §2.1 y las tablas de estados de §3.4 y §3.6 de **este
    documento** y los pasa por el backend real, y lo mismo con el REST de §6. Si se cambia el
    contrato sin el código, o al revés, `make test` falla.
14. Sin objeto desde la v4.0: la MAC lógica del gateway y las salas. Con ESP-WIFI-MESH los nodos
    encuentran al raíz por el ID de la mesh ([ADR-006](adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md)).
15. **Custodia de credenciales y configuraciones**: el `config_local.h` del raíz (WiFi del
    edificio, clave de la mesh, broker), las AppKeys de LoRaWAN y la clave de la mesh que se le da
    a cada grupo. No pueden ir a `aura-firmware`, que es público. Opciones: repositorio privado de
    la cátedra o gestor de secretos.
16. Sin objeto desde la v4.0: el descubrimiento por baliza firmada. ESP-WIFI-MESH descubre y
    elige padre sola, y la autenticación es la clave de la mesh (pendiente 8).
17. **Prueba en placa de la mesh ESP-WIFI-MESH**. ✅ De punta a punta en ESP-IDF el 2026-10-20
    ([ADR-007](adr/ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md), `aura-firmware/Mesh_Red_Completa/Broker_MQTT/LEEME.md`);
    falta una prueba larga. Lo que sigue es el plan original, en Arduino
    (`aura-firmware/docs/mesh-wifi/estado-de-banco.md`): una hoja que se une sin la clave del WiFi
    del edificio, el raíz con IP y MQTT con la mesh andando, la MAC de origen a dos saltos, la
    bajada por MAC y el reenganche después de reiniciar el raíz. Si los dos primeros fallan, se
    reabre [ADR-006](adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md).
18. **LoRaWAN pierde la muestra si falla la base** (tema de estudio). Si el `INSERT` falla por
    un error transitorio, el backend hace rollback y no publica `ack` (§3.6): en la mesh la hoja
    reintenta, pero en LoRaWAN nadie lo hace. El nodo no guarda la muestra, ChirpStack ya la
    entregó y el bridge no escucha el `ack`, así que la muestra se pierde sin aviso. Lo mismo pasa
    si el bridge se reinicia mientras el broker AURA está caído: paho guarda los QoS 1 pendientes
    solo en memoria. **Aceptado por ahora**: las caídas de la base son raras y una muestra LoRaWAN
    perdida no se repone de ningún modo en el nodo. **Para arrancar**: que el bridge se suscriba a
    `hw/eui-+/ack` (la ACL lo tendría que permitir), guarde en disco los `data` publicados sin
    `ack` y los republique con el mismo `ingest_id` (el backend contesta `duplicado` si ya
    estaban). Medir antes: contar uplinks contra acks durante un mes.
19. **Codecs que meten diagnóstico en `values`** (tema de estudio). Un codec del ChirpStack del
    banco devolvía en `object` el `contador` de tramas, `null` para la sonda sin eco y textos de
    estado (`tanque2_estado: "sin_eco"`). El bridge pasa `object` tal cual (§3.1), así que las tres
    cosas entraban a la serie como mediciones, contra lo que pide §3.1, y nadie fallaba.
    ✅ **Ese codec está corregido** (2026-10-23): es el del profile `aura-tanque-3x-ultrasonico`
    (antes `aura-clase-a`), que omite el tanque sin lectura válida, no manda contador y, si ningún
    tanque da lectura, no publica `data`. La falla queda en los `warnings` del evento de
    ChirpStack, que AURA no ve.
    **Sigue aceptado**: el bridge no valida `values`, porque su forma la define cada codec.
    **Falta**: mandar la falla de la sonda como `alerts/sensor` desde un uplink en otro puerto, y
    que el conector de cada dispositivo rechace en el banco los campos que no declara en su
    `conector.toml`.

20. **Asignar el aula desde AURA** (v4.1, §2.3 y §3.3). Hoy se hace mandando a mano
    `set_config {"aula": …}` por la API de comandos. Falta que AURA lo haga sola al asignar una
    placa a un dispositivo, sacando el aula del recinto del dispositivo, y que lo repita al
    reemplazar la placa. Hasta entonces, una placa recién asignada sigue en `modo: "sin_aula"`
    (manda directo al raíz, sus datos llegan igual).
21. **El CI de `aura-firmware` no compila los proyectos ESP-IDF** (`Mesh_Red_Completa/`): solo los
    sketches de Arduino. Falta un job con la imagen `espressif/idf` que haga `idf.py build` de los
    tres proyectos y de cada hoja de `dispositivos/`.
22. **Actualización por la malla desde AURA** (v4.3, §3.3). En el banco la hace
    `aura-firmware/Mesh_Red_Completa/Broker_MQTT/ota.py`. Falta que AURA: guarde y sirva los `.bin`
    por HTTPS (con su SHA-256), muestre `details.firmware` de cada placa, mande `ota` a una placa o a
    todas las de un tipo (y aula), siga las `response` y vuelva a pedir lo que no terminó si el raíz
    se reinicia (su `status` vuelve con `uptime_s` menor) o si se cortó el broker.
23. **Imágenes firmadas** ([ADR-008](adr/ADR-008-actualizacion-por-la-malla-y-tipo-de-firmware.md)).
    ✅ Desde la v4.4 cada placa verifica, al instalarla, que la imagen esté firmada con la clave de la
    cátedra (sin eFuses). **Falta:** decidir quién custodia la clave de firma (pendiente 15) y, en las
    placas de la instalación final, Secure Boot V2 para que la firma se verifique también en cada
    arranque (quema eFuses).

De la v1.x quedan resueltos el destino de los diagnósticos del gateway (van a `status.details`) y
`pend`/`desc` (pasan a `pendientes`/`descartadas` en `status`). `lux_sim` sigue sin objeto.
