# ADR-002 — Los primeros dispositivos de AURA van por LoRaWAN, no por la mesh ESP-NOW

**Estado:** aceptada · **Fecha:** 2026-09-25 · **Decide:** Matías Wanzenried
**Afecta a:** `docs/CONTRATO_MQTT.md` (pasa a v2.0), `app/backend/services/device_integration.py`,
`firmware/` de este repositorio y el repositorio público `aura-firmware`.

## El problema

Entre el 2026-08-28 y el 2026-09-04 se construyó la capa de dispositivos de AURA como una **mesh
ESP-NOW** de dos saltos, `sensor → sala → gateway`, en placas XIAO ESP32S3. El gateway era el
único nodo con WiFi y el único que hablaba MQTT. El diseño está en
`docs/superpowers/specs/2026-08-28-firmware-mesh-espnow-design.md`.

El 2026-09-11 se armó en paralelo un **banco LoRaWAN**: nodo XIAO ESP32S3 + Wio-SX1262, gateway
Milesight UG63-915M y ChirpStack v4 en Docker (repositorio público `IC-lorawan-test`). Funcionó de
punta a punta en el aire, con cero pérdidas en la primera prueba dentro del edificio y ~19 dB de
margen sobre el límite de SF7.

Con los dos bancos andando, había que elegir por dónde van los **primeros dispositivos** del
proyecto, que tienen que **enviar y recibir**.

## Qué pesó

| | Mesh ESP-NOW | LoRaWAN |
|---|---|---|
| Alcance | un salto de radio por nodo; cubrir el campus exige nodos de sala intermedios | kilómetros por salto; un gateway cubre el edificio con margen medido |
| Infraestructura | el gateway depende del WiFi de invitados (`UNRaf_Libre`, IP por DHCP que cambia entre jornadas) | el gateway tiene su propio enlace; el nodo no toca el WiFi |
| Protocolo propio | trama, ACK, reintentos y deduplicación escritos a mano; la §9 del spec registra cinco defectos que aparecieron **solo en hardware** pese a 1.084 verificaciones de host | estandarizado; ChirpStack resuelve sesión, ACK, ADR y deduplicación entre gateways |
| Seguridad de radio | sin cifrado: `encrypt = false` en los tres nodos, fuera de alcance por diseño | AES por dispositivo (OTAA) |
| Identidad del dato | `ingest_id` solo en el camino REST; por MQTT, deduplicación en RAM | `deduplicationId` de origen en cada uplink |
| Bajada | inmediata, hasta 180 B | clase A: con el próximo uplink; clase C: inmediata; payload chico y variable según el *data rate* |
| Configuración | la trampa está en el firmware | la trampa está en ChirpStack: casi todo lo mal configurado falla en silencio |

La desventaja real de LoRaWAN es la bajada: latencia en clase A y payloads chicos. Se resuelve
con clase C para los actuadores, que están alimentados de red, y con un límite de downlink
explícito por tipo de dispositivo.

## Opciones para llevar los datos de ChirpStack a AURA

ChirpStack publica `application/<appId>/device/<devEUI>/event/up` con su propio JSON, e
identifica por DevEUI. El contrato de AURA exige `devices/<uuid>/data` con `{"values": …}`.

**A. Adaptador propio (`lorawan-bridge`)**: se suscribe a los eventos de ChirpStack, traduce y
publica en el broker de AURA. El backend y el árbol de tópicos no cambian. Cuesta un proceso más
para operar.

**B. Integración HTTP de ChirpStack → `POST /api/v1/telemetry/ingest`**: sin proceso nuevo. Pero
ChirpStack postea **su** formato, no el de AURA, así que igual hace falta un traductor, ahora
adentro del backend. Además no resuelve la bajada.

**C. El backend se suscribe directo a `application/#`**: sin proceso nuevo, pero el backend queda
atado al formato de un proveedor y conviven dos árboles de tópicos en el mismo código.

**Se elige A.** Es la única que deja el contrato intacto para el backend: para él, el bridge es un
gateway más, como lo era el de la mesh. Y establece un patrón que el proyecto ya necesita: **un
adaptador por protocolo que publica en `devices/<uuid>/…`**, que es justamente lo que ADR-001
recomienda para el medidor Circutor.

**Mapeo DevEUI → UUID.** Cada dispositivo lleva en ChirpStack el tag `aura_device_id` con su UUID
de AURA. ChirpStack manda los tags en cada evento, así que el bridge traduce sin tabla propia, y el
mapeo vive donde se da de alta el dispositivo. Se descartó una tabla en el bridge o en la base de
AURA, porque el dato quedaría en dos lugares que pueden desincronizarse.

## La decisión

1. Los primeros dispositivos de AURA son **nodos LoRaWAN** gestionados por ChirpStack, y **envían
   y reciben**.
2. Los datos llegan a AURA por el **`lorawan-bridge`**, con el tag `aura_device_id` como
   identidad. La especificación normativa es `docs/CONTRATO_MQTT.md` v2.0.
3. La **mesh ESP-NOW queda congelada, no eliminada.** No se usa en los primeros dispositivos, pero
   no se borra nada:
   - su código vive en el repositorio **público `aura-firmware`**, con `main` protegida (solo por
     pull request), como copia limpia, sin credenciales;
   - `firmware/` de este repositorio, la rama `feature/firmware-mesh` y su worktree quedan como
     están; `firmware/README.md` avisa que esa copia está congelada;
   - el spec, el plan y el estado de banco de `docs/superpowers/` quedan como registro histórico.

## Qué se gana y qué se pierde

**Se gana:**
- alcance medido en lugar de nodos repetidores;
- independencia del WiFi de invitados;
- deduplicación con identificador de origen;
- cifrado de radio;
- tópicos de AURA que se cambian en el bridge sin reflashear nada.

**Se pierde:**
- la bajada inmediata en clase A;
- el control total sobre el protocolo;
- el trabajo de verificación de la mesh, que queda congelado.

Además, **el gateway LoRaWAN pasa a ser un punto único de falla**: si se cae, no llega ningún
dispositivo. Con la mesh pasaba lo mismo con su gateway, pero hay que tenerlo presente al
cablearlo y alimentarlo.

**Costo de la transición en el backend.** `device_integration.py` deja de limitar los comandos a
180 B (ese límite era de la trama ESP-NOW) y pasa a aceptar solo los estados de `response` del
bridge. Un gateway de la mesh que volviera a publicar `enviado_a_mesh` sería rechazado; por eso el
cambio de contrato es a v2.0.

## Lo que esta decisión NO resuelve

- **La ingesta MQTT del backend no persiste nada hoy** (`CONTRATO_MQTT.md` §10, pendiente 1). Es
  previo a esta decisión e independiente de ella, pero la bloquea: sin eso, el bridge publica al
  vacío.
- **Dónde se despliega ChirpStack en producción.** Hoy vive en `IC-lorawan-test`, que es un
  repositorio didáctico y público. El despliegue de AURA necesita su propia configuración, sin
  secretos versionados.
- **Qué hardware lleva cada tipo de nodo.** Las tres XIAO del banco de la mesh sirven para LoRaWAN
  solo con un Wio-SX1262 cada una.

## Disparador de revisión

Reabrir si pasa alguna de estas cosas:
- aparece un caso de uso que necesite **muchos mensajes por minuto** o payloads grandes de bajada
  que la región AU915 no permite;
- la cobertura medida fuera del edificio no alcanza con uno o dos gateways;
- el costo de operar ChirpStack supera al de mantener el protocolo propio de la mesh.
