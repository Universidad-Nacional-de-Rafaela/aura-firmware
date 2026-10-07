# ADR-003 — Dos transportes: mesh ESP-NOW en interior, LoRaWAN en exterior

**Estado:** propuesta · **Fecha:** 2026-10-05 · **Decide:** Matías Wanzenried
**Reemplazado en parte por:** [ADR-004](ADR-004-mqtt-punto-comun-de-ingesta-con-ack-de-persistencia.md)
(el punto 3: la telemetría de la mesh pasa a MQTT, con `ack` de persistencia del backend).
**Reemplazado en parte por:** [ADR-006](ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md)
(la tecnología de la mesh: ESP-WIFI-MESH en lugar de ESP-NOW). El criterio interior/exterior sigue.
**Reemplaza en parte a:** [ADR-002](ADR-002-lorawan-reemplaza-a-la-mesh-espnow.md) (el punto 3,
"la mesh queda congelada"). Los puntos 1 y 2 de ADR-002 siguen vigentes para los dispositivos de
exterior.
**Afecta a:** `docs/CONTRATO_MQTT.md` (pasa a v3.0), `app/backend/services/device_integration.py`,
`app/backend/api/endpoints/telemetry.py`, `firmware/` de este repositorio y el repositorio público
`aura-firmware`.

## El problema

ADR-002 eligió LoRaWAN para los primeros dispositivos y congeló la mesh ESP-NOW. Diez días
después aparecieron dispositivos para los que esa decisión no encaja:

- Un grupo de IC IV migró su nodo de monitoreo de heladeras y freezers desde LoRaWAN a la mesh,
  sobre la base de `aura-firmware`. Es un equipo **de interior**: está en un laboratorio, a
  pocos metros de otros nodos, con WiFi en el edificio y alimentación de red. Necesita
  configuración remota con respuesta en segundos y reporte de la configuración aplicada.
- Con LoRaWAN, ese mismo equipo paga **latencia de clase A** (la configuración llega con el
  próximo uplink, que puede tardar minutos) y **payloads de bajada de pocos bytes** en AU915 con
  *data rates* bajos. Además necesita un Wio-SX1262 por nodo.

ADR-002 ya había identificado esas dos desventajas, pero las resolvió pensando en actuadores
alimentados de red (clase C). Los sensores de interior también tienen alimentación de red y no
necesitan alcance de kilómetros, así que para ellos los costos de LoRaWAN no tienen contrapartida.

## Opciones

**A. Todo por LoRaWAN (lo que dice ADR-002).** Un solo transporte y un solo adaptador. Los
dispositivos de interior quedan con bajada lenta y chica, y cada nodo lleva radio LoRa.

**B. Todo por mesh.** Bajada inmediata y sin hardware extra, pero no llega al exterior ni a
edificios alejados sin una cadena de repetidores. Además depende del WiFi del edificio para el
gateway. Para el exterior fue justamente lo que descartó ADR-002.

**C. Dos transportes según dónde está el dispositivo.** Mesh en interior, LoRaWAN en exterior o
donde la distancia lo exija. Cada transporte tiene su **adaptador**, que publica en el mismo
árbol `devices/<uuid>/…`. El backend no distingue de dónde vino un dato.

**Se elige C.** Es el patrón que ADR-002 ya había establecido ("un adaptador por protocolo que
publica en `devices/<uuid>/…`") y que ADR-001 recomienda para el Circutor. Con C la mesh es
un adaptador más, no una arquitectura paralela.

## La decisión

1. **Criterio de asignación.** Un dispositivo va por **mesh** si está dentro de un edificio con
   cobertura WiFi para el gateway de la mesh y a un salto (o a un salto más un nodo de sala) de
   ese gateway. Va por **LoRaWAN** si está en exterior, en un edificio sin gateway de mesh, o si
   necesita más de dos saltos. Ante la duda, LoRaWAN: alcanza más y no depende del WiFi.
2. **Dos adaptadores, un contrato.** El `lorawan-bridge` (ADR-002) y el **gateway de la mesh**
   publican en el mismo árbol de tópicos. El contrato MQTT v3.0 especifica qué hace cada uno.
3. **La telemetría de la mesh entra por REST**, por `POST /api/v1/telemetry/ingest`, no por
   `devices/<id>/data`. El sensor de la mesh guarda cada muestra hasta que AURA confirma que la
   persistió. Esa confirmación solo la puede dar el REST: un PUBACK de MQTT solo dice que el
   broker recibió el mensaje. Estado, comandos y respuestas sí van por MQTT, igual que LoRaWAN.
4. **Un dispositivo de AURA es una placa**, en los dos transportes. Un nodo con varias sondas es
   **un** `device_id`, y cada sonda es un campo de `values` (por ejemplo, `temp_heladera_c` y
   `temp_freezer_c`). Los parámetros del nodo (intervalo de recuperación, canal) se configuran
   sobre ese único `device_id`.
5. **La mesh se descongela**, pero su firmware implementa la v1.x del contrato. Hasta que se lo
   actualice a la v3.0, ningún gateway de la mesh se conecta al broker de producción.

## Qué se gana y qué se pierde

**Se gana:**
- bajada inmediata y de hasta 180 B para los dispositivos de interior;
- nodos de interior más baratos, sin radio LoRa;
- confirmación de persistencia de punta a punta para la mesh, por REST;
- el trabajo de verificación de la mesh (spec, plan, estado de banco) vuelve a ser útil.

**Se pierde:**
- simplicidad: hay **dos adaptadores** para mantener, dos firmwares y dos maneras de dar de alta
  un dispositivo (tag en ChirpStack o tabla en el gateway de la mesh);
- simetría en la ingesta: la telemetría LoRaWAN entra por MQTT y la de la mesh por REST. Hasta
  que la ingesta MQTT deduplique por `ingest_id`, los dos caminos no tienen las mismas
  garantías;
- la mesh **no cifra la radio** (`encrypt = false`); LoRaWAN sí. Los dispositivos de interior
  quedan con menos seguridad de radio hasta que se active el cifrado de ESP-NOW.

El gateway de la mesh **depende del WiFi del edificio** (`UNRaf_Libre`, con IP por DHCP). Si el
WiFi se cae, el gateway no confirma nada y los sensores guardan sus muestras hasta que vuelva,
dentro de la capacidad de su cola.

## Lo que esta decisión NO resuelve

- **La ingesta MQTT del backend sigue sin persistir nada** (`CONTRATO_MQTT.md` §10, pendiente 1).
  Afecta al estado y a la telemetría LoRaWAN, no a la telemetría de la mesh, que entra por REST.
- **El endpoint REST pierde inserciones en un batch con un duplicado** (`CONTRATO_MQTT.md` §6).
  Hasta corregirlo, el gateway de la mesh manda un evento por request.
- **Dónde vive la tabla MAC → UUID** a largo plazo. Por ahora está en la configuración del
  gateway de la mesh; agregar un sensor requiere reflashearlo.
- **El cifrado de ESP-NOW** (PMK/LMK) y su gestión de claves.

## Disparador de revisión

Reabrir si pasa alguna de estas cosas:
- el WiFi del edificio resulta demasiado inestable para el gateway de la mesh;
- mantener dos adaptadores cuesta más que aceptar la bajada lenta de LoRaWAN en interior;
- aparece un dispositivo de interior que necesita más de dos saltos de mesh.
