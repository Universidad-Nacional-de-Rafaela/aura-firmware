# ADR-005 — Identidad por placa: los adaptadores publican con el `hw_id` y AURA hace el mapeo

**Estado:** propuesta · **Fecha:** 2026-10-07 (decisiones del 2026-10-05) · **Decide:** Matías Wanzenried
**Reemplaza en parte a:** [ADR-004](ADR-004-mqtt-punto-comun-de-ingesta-con-ack-de-persistencia.md)
(el punto 5, "un `device_id` desconocido no se confirma"). El resto de ADR-004 sigue vigente:
MQTT es el único camino de entrada y el backend confirma con un `ack` después del commit.
**Afecta a:** `docs/CONTRATO_MQTT.md` (pasa a v4.0: árbol `hw/`), `app/backend/services/`, el
modelo `devices`, `ts_telemetry`, el `lorawan-bridge` y el raíz de la mesh en `aura-firmware`.

## El problema

Hasta la v3.x del contrato, los tópicos llevan el **UUID de AURA** (`devices/<uuid>/…`). El UUID
se crea en AURA y hay que **copiarlo a mano** a donde vive el adaptador: el tag `aura_device_id`
de ChirpStack, la tabla MAC → UUID del gateway de la mesh y, para el reemplazo de placas, la MAC
lógica del gateway y de las salas. Al relevar el alta completa de un dispositivo (2026-10-05)
aparecieron tres consecuencias:

- **Un UUID mal copiado destruye datos sin error.** El dato se guarda bajo un dispositivo
  fantasma (ADR-004 agregó la validación) o queda sin confirmar en la cola del nodo hasta que
  alguien corrige la tabla y reflashea.
- **Agregar un nodo de la mesh obliga a reflashear el gateway**, porque la tabla vive en su
  `config_local.h`.
- **Una placa quemada se lleva su identidad.** Para que un repuesto ocupe su lugar, la cátedra
  tenía que custodiar la configuración de cada gateway y cada sala (contrato v3.1, §2.4).

## Opciones

**A. Seguir con el UUID en el tópico y agregar el `hw_id` al payload.** Ayuda a diagnosticar,
pero la copia manual del UUID, la tabla del gateway y el reflasheo siguen.

**B. Los adaptadores publican con la identidad de la placa y el mapeo pasa a AURA.** El UUID no
cruza la frontera con los dispositivos. AURA traduce `hw_id → dispositivo` con un dato que se
edita en la plataforma, sin tocar firmware.

**Se elige B.**

## La decisión

1. **Identidad por placa.** Cada mensaje viaja con el `hw_id` de la placa:
   `mac-<12 hex>` (MAC de fábrica, mesh), `eui-<16 hex>` (DevEUI, LoRaWAN) o `svc-<nombre>`
   (un adaptador que no es una placa, como el `lorawan-bridge`). Siempre en minúsculas:
   `^(mac-[0-9a-f]{12}|eui-[0-9a-f]{16}|svc-[a-z0-9-]{1,32})$`.
2. **Árbol `hw/`** (contrato v4.0): `hw/<hw_id>/{data, status, response, command, ack,
   alerts/<tipo>}`, QoS 1, `status` retenido.
3. **Cuarentena.** Lo que llega de un `hw_id` que ningún dispositivo tiene asignado se guarda
   igual en `ts_telemetry`, con `device_id` y `tenant_id` nulos, y se confirma con
   `resultado: "cuarentena"`: el nodo libera su cola. Se registra el mensaje completo en el log.
   La cuarentena se conserva 30 días.
4. **Placas ignoradas.** Una placa marcada como ignorada (de banco, de pruebas) se confirma con
   `descartado` y no se guarda.
5. **Asignar** una placa a un dispositivo pone `devices.hw_id` y reclama la cuarentena de ese
   `hw_id` desde una fecha opcional. Un dispositivo tiene **una placa a la vez**: asignar otra
   placa a un dispositivo que ya tenía una es el **reemplazo de placa**, y la vieja queda
   desasignada. El dispositivo conserva su UUID y su historial.
6. **El raíz de la mesh no tiene tabla.** Publica todo lo que le llega con el `hw_id` de origen
   y no valida qué comandos acepta cada nodo: el nodo rechaza los que no conoce.
7. **LoRaWAN:** el `lorawan-bridge` publica con `eui-<DevEUI>` y no necesita el tag
   `aura_device_id`. Los dos nodos que ya están dados de alta en ChirpStack conservan su DevEUI.

## Qué se gana y qué se pierde

**Se gana:**
- el alta de un dispositivo es un solo paso, en AURA: asignar el `hw_id` que imprime
  `herramientas/leer_mac` (o el DevEUI) al dispositivo;
- agregar un nodo a la mesh no toca la configuración del raíz;
- reemplazar una placa es reasignar, sin custodiar configuraciones ni MAC lógicas;
- un error en el alta deja de perder datos: van a cuarentena y se reclaman al corregir.

**Se pierde:**
- **cambia el árbol de tópicos**, que es el cambio más caro del contrato (§7 regla 3). Hoy
  ningún adaptador publica en producción, así que no hay transición que sostener;
- el backend carga con más lógica: búsqueda por `hw_id`, cuarentena, asignación, purga;
- la cuarentena guarda datos de placas que quizás nunca se asignen: hace falta purgarla;
- la identidad de los dos nodos LoRaWAN existentes es un DevEUI asignado a mano, no de fábrica.

## Lo que esta decisión NO resuelve

Se implementa en el MVP de ingesta (`docs/superpowers/plans/2026-10-05-mvp-ingesta-hw.md`,
Tasks 1 a 8). Quedan afuera, con su paso obligatorio o debilidad aceptada:

- la interfaz de placas descubiertas y retiradas (en el MVP: `GET /api/v1/hw/descubiertas`);
- la asignación por la hora de la muestra: antes de retirar una placa que funciona, esperar a
  que su cola quede en 0 (`pendientes` del reporte);
- la bitácora del dispositivo (cambio de placa o sonda, recalibración, reubicación), la
  calibración y el perfil de medición: el **UUID como dispositivo lógico** sigue en discusión;
- la purga automática de la cuarentena (en el MVP: `scripts/purgar_cuarentena.py`).

## Disparador de revisión

Reabrir si pasa alguna de estas cosas:
- la cuarentena crece sin que nadie la asigne ni la purgue;
- aparece un transporte cuya identidad de placa no es estable (por ejemplo, MAC aleatorias);
- hace falta que un adaptador conozca el dispositivo lógico (para validar comandos, por ejemplo).
