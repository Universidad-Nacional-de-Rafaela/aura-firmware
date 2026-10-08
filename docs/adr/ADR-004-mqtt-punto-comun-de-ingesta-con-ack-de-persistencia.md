# ADR-004 — MQTT como punto común de ingesta, con ack de persistencia del backend

**Estado:** propuesta · **Fecha:** 2026-10-05 · **Decide:** Matías Wanzenried
**Reemplaza en parte a:** [ADR-003](ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) (el punto
3, "la telemetría de la mesh entra por REST"). El resto de ADR-003 sigue vigente.
**Reemplazado en parte por:** [ADR-005](ADR-005-identidad-por-placa-y-mapeo-en-aura.md) (el punto 5:
un dispositivo desconocido va a cuarentena y se confirma con `cuarentena`). Los tópicos de este ADR
(`devices/<id>/…`) pasan a `hw/<hw_id>/…` en el contrato v4.0; la v3.1 no se publicó.
**Afecta a:** `docs/CONTRATO_MQTT.md` (v3.1, absorbida por la v4.0), `app/backend/services/device_integration.py`,
`app/backend/services/mqtt_service.py`, el gateway de la mesh en el repositorio público
`aura-firmware` y el `lorawan-bridge`.

## El problema

ADR-003 dejó dos caminos de entrada para la telemetría: LoRaWAN por MQTT (`devices/<id>/data`, a
través del `lorawan-bridge`) y la mesh por REST (`POST /api/v1/telemetry/ingest`). El motivo era
uno solo: el nodo de la mesh guarda cada muestra en flash hasta que AURA confirma que la
**persistió**, y en ADR-003 esa confirmación solo la daba la respuesta del REST. Un PUBACK de MQTT
dice que el broker recibió el mensaje, no que el backend lo guardó.

Al relevar el pipeline completo de alta de un dispositivo (2026-10-05) aparecieron los costos de
tener dos caminos:

- **Dos validaciones, dos deduplicaciones, dos lugares donde fallar.** El REST deduplica por
  `ingest_id`; la ingesta MQTT, no. El REST tiene dos defectos documentados (contrato §6): pierde
  eventos de un batch con error y devuelve `201` aunque todo haya fallado.
- **Nadie valida que el `device_id` exista.** `ts_telemetry` vive en otra base, sin clave foránea a
  `devices`. Un UUID mal copiado en la tabla del gateway o en el tag de ChirpStack persiste datos
  bajo un dispositivo fantasma, sin error. Con dos caminos, esa validación habría que escribirla
  dos veces.
- **El gateway de la mesh carga con un cliente HTTP** con su propio `WiFiClient`, un timeout de
  2,5 s que tiene que quedar por debajo del ACK de la sala (4 s) y su propia configuración
  (`API_BASE`, `API_TOKEN`).
- **El backend no ve un solo flujo de dispositivos.** Estado, comandos y alertas de la mesh ya van
  por MQTT; solo la telemetría se desvía.

## Opciones

**A. Dejar el REST para la mesh (ADR-003).** Funciona hoy para la telemetría de la mesh. Mantiene
los dos caminos y sus costos.

**B. La mesh publica en `devices/<id>/data` y el gateway confirma al nodo con el PUBACK**, con el
backend conectado con sesión persistente y Mosquitto guardando en disco. Es lo más simple, pero la
durabilidad pasa de la flash del nodo al disco del broker, y un mensaje que el backend recibe y no
logra guardar se pierde sin que nadie lo sepa. Es justo el modo de falla que el contrato intenta
eliminar.

**C. La mesh publica en `devices/<id>/data` y el backend confirma por MQTT después de guardar**, en
un tópico nuevo `devices/<id>/ack` con el `ingest_id`. El gateway de la mesh le manda la
CONFIRMACION al nodo recién cuando llega ese ack.

**Se elige C.** Conserva la confirmación de punta a punta de ADR-003, pero por el mismo camino que
LoRaWAN.

## La decisión

1. **Toda la telemetría de dispositivos entra por MQTT**, en `devices/<device_id>/data`. Los dos
   adaptadores publican ahí con el mismo payload. `POST /api/v1/telemetry/ingest` deja de ser un
   camino de dispositivos; queda para cargas manuales y herramientas.
2. **El backend confirma lo que persistió** en `devices/<device_id>/ack`, con
   `{"ingest_id", "resultado"}`, **después del commit** en la base:
   - `persistido` o `duplicado`: la muestra está en la base. Para el adaptador son equivalentes.
   - `rechazado`, con `motivo`: la muestra nunca va a poder persistirse (por ejemplo, `values` no es
     un objeto). Se confirma igual para no trabar la cola del nodo.
   - **Sin ack** ante un error transitorio (base caída, por ejemplo). El nodo reintenta solo.
3. **El gateway de la mesh confirma al nodo solo con un ack** `persistido`, `duplicado` o
   `rechazado` de ese `ingest_id`. El PUBACK, el ACK de radio y el de la sala siguen sin ser
   confirmación.
4. **`ingest_id` pasa a ser obligatorio** en `data` para la mesh. Sin él no hay ack posible.
5. **Un `device_id` que no está en `devices` no se confirma.** El backend:
   - no persiste el dato ni publica ack: en la mesh, la muestra queda en la cola del nodo y se
     reintenta, así que corregir la tabla del gateway recupera los datos;
   - **registra cada mensaje completo** en el log (tópico, `device_id`, `adaptador`, `ingest_id`,
     `ts` y `values`), para poder reinyectarlo a mano: en LoRaWAN no hay cola en el nodo y el log
     es la única copia;
   - **publica una alerta** `alerts/<adaptador>/dispositivo_desconocido` con el `device_id` en
     `details`, **una vez por cambio**: la primera vez que aparece ese `device_id` desde ese
     adaptador, y otra con `severity: "info"` y `motivo: "recuperado"` cuando el dispositivo se da
     de alta y su primer dato se persiste.
6. **Los dos adaptadores identifican de dónde viene el dato** con un campo `adaptador` (el
   `device_id` del bridge o del gateway de la mesh) en `data`. Es lo que permite atribuir la alerta
   del punto 5 a un dispositivo que existe.

## Qué se gana y qué se pierde

**Se gana:**
- un solo camino de entrada, con una sola validación, una sola deduplicación por `ingest_id` y una
  sola lectura de `ts`;
- la validación del `device_id` contra `devices`, que hoy no hace ningún camino;
- un error de tipeo en el alta deja de destruir datos: en la mesh quedan en la cola del nodo, en
  LoRaWAN quedan en el log, y en los dos casos hay una alerta;
- un gateway de la mesh más simple: sin cliente HTTP, sin `API_BASE` ni `API_TOKEN`, sin la
  restricción de timeouts entre el POST y el ACK de la sala;
- los dos defectos del endpoint REST dejan de afectar a los dispositivos.

**Se pierde:**
- **la mesh deja de poder probarse sin la ingesta MQTT del backend.** Con ADR-003 la telemetría de
  la mesh llegaba a la base aunque la ingesta MQTT no funcionara. Ahora los pendientes 1 (que la
  ingesta persista) y 6 (deduplicar por `ingest_id` y leer `ts`) del contrato bloquean los dos
  transportes. Ya eran obligatorios para LoRaWAN;
- un tópico más en el contrato y un consumidor más del lado del gateway;
- **el backend pasa a publicar**, no solo a consumir, en el árbol `devices/` (ya lo hacía con
  `command`). La ACL del broker (pendiente 11) tiene que reservar `devices/+/ack` para el backend:
  quien pueda publicar ahí puede hacer que un nodo borre muestras que no se guardaron.

**Reinicios del backend.** Mientras el backend no conecte con `client_id` fijo y sesión persistente
(pendiente 7), lo que llegue durante un reinicio se pierde en el broker. En la mesh no hay pérdida:
sin ack, el nodo reintenta. En LoRaWAN sí, igual que antes de esta decisión.

**Dispositivo desconocido por más de 24 h.** La cola del nodo de la mesh guarda 1440 muestras (24 h
a una por minuto). Si nadie corrige el alta en ese plazo, el nodo empieza a pisar las más viejas y
las cuenta en `descartadas`. Igual quedan en el log del backend.

## Lo que esta decisión NO resuelve

- **Que la ingesta MQTT persista** (contrato §10, pendiente 1). Esta decisión la vuelve más urgente.
- **El alta en dos lugares.** El UUID se sigue copiando a mano a la tabla del gateway de la mesh o al
  tag de ChirpStack. La validación del punto 5 detecta el error; no lo evita.
- **Qué pasa con LoRaWAN cuando el backend no confirma.** El bridge ignora el ack: ChirpStack no
  retransmite y el nodo LoRa no guarda muestras. El ack sirve para medir lo que se perdió, no para
  recuperarlo.
- **Los dos defectos del endpoint REST** siguen ahí para quien lo use a mano.

## Disparador de revisión

Reabrir si pasa alguna de estas cosas:
- el ack agrega una latencia que llena la cola de los nodos en condiciones normales;
- la ingesta MQTT del backend resulta inestable y la mesh necesita volver a un camino que no
  dependa de ella;
- aparece un tercer adaptador que no puede esperar un ack (por ejemplo, el Circutor de ADR-001), y
  conviene revisar si el ack es por adaptador o general.
