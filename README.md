# aura-firmware

Firmware de los dispositivos del proyecto **AURA** (Administración Unificada de Recursos y
Accesos), Universidad Nacional de Rafaela. Acá vive el código que corre **en las placas**: la
infraestructura de la mesh que mantiene la cátedra y el firmware de cada dispositivo que
desarrollan los grupos de Ingeniería en Computación III y IV.

La plataforma (backend y web) está en otro repositorio. Lo único que une a los dos es el
**contrato**: [`docs/CONTRATO_MQTT.md`](docs/CONTRATO_MQTT.md). Si un dispositivo no lo
respeta, AURA no falla: descarta el mensaje en silencio.

## Dos transportes

Según [ADR-003](docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md):

- **Interior → mesh ESP-NOW.** El dispositivo habla con un nodo de sala o directo con el
  gateway de la mesh, que es el único con WiFi.
  ```
  dispositivo --ESP-NOW--> nodo_sala --ESP-NOW--> nodo_gateway --WiFi--> AURA
  ```
- **Exterior o lejos → LoRaWAN**, por el gateway LoRa y ChirpStack. El banco del aula y un
  nodo de ejemplo están en
  [`IC-lorawan-test`](https://github.com/Universidad-Nacional-de-Rafaela/IC-lorawan-test).

## Estructura

| Carpeta | Qué hay | Quién la mantiene |
|---|---|---|
| [`comun/`](comun/) | Trama de la mesh, buffer circular, `ingest_id`, y sus tests de host | cátedra |
| [`infraestructura/`](infraestructura/) | `nodo_gateway` y `nodo_sala` de la mesh | cátedra |
| [`dispositivos/`](dispositivos/) | **Una carpeta por dispositivo**, más `plantilla_dispositivo/` y `sensor_ejemplo/` | cada grupo |
| [`herramientas/`](herramientas/) | `broker.sh`, `leer_mac/`, `generar_autocontenidos.sh` | cátedra |
| [`autocontenido/`](autocontenido/) | Copia de cada sketch con los headers de `comun/` al lado, para abrir en el IDE. **Se genera, no se edita** | — |
| [`docs/`](docs/) | Copia publicada del contrato, ADRs y el histórico de la mesh v1 | cátedra |

## Estado

> ⚠️ **`infraestructura/` y `sensor_ejemplo/` implementan la versión 1.x del contrato**, que
> AURA ya no acepta (telemetría por MQTT, `enviado_a_mesh`, sin cola persistente en el nodo).
> Sirven como referencia de ESP-NOW y para el banco, pero **no se conectan a AURA tal cual**.
> Su actualización a la v3.0 está pendiente (contrato §10).

## Agregar un dispositivo

Ver [`CONTRIBUTING.md`](CONTRIBUTING.md). En corto: fork, copiar `dispositivos/plantilla_dispositivo/`,
completar su README, y abrir un pull request. El CI compila, corre los tests y busca secretos;
si algo falla, el PR no se puede mergear.

## Compilar

Placa **XIAO_ESP32S3** con **USB CDC On Boot: Enabled** (sin eso el monitor serie queda mudo),
core `esp32:esp32` 3.3.11. Con `arduino-cli`:

```bash
arduino-cli compile --fqbn "esp32:esp32:XIAO_ESP32S3:CDCOnBoot=cdc" infraestructura/nodo_gateway
```

Los sketches incluyen `comun/` con rutas relativas (`../../comun/…`), y así compilan tanto en
`arduino-cli` como en el IDE 2. Si preferís abrir una carpeta suelta, usá la de `autocontenido/`.

Tests de host de `comun/`, sin placa:

```bash
make -C comun/tests
```

## Seguridad

**Este repositorio es público.** Credenciales de WiFi, claves de LoRaWAN (AppKey), tokens y
MAC de producción van en `config_local.h`, que está en el `.gitignore`. El CI rechaza cualquier
PR que los versione. Si algo se filtra igual, avisá a la cátedra: borrarlo en un commit nuevo
**no alcanza**, porque queda en la historia; hay que cambiar la clave.

`main` está protegida: los cambios entran solo por pull request.
