# aura-firmware

Firmware de los dispositivos del proyecto **AURA** (Administración Unificada de Recursos y
Accesos), Universidad Nacional de Rafaela. Acá vive el código que corre **en las placas**: la
infraestructura de la mesh que mantiene la cátedra, y una carpeta por cada dispositivo que
desarrollan los grupos de Ingeniería en Computación III y IV, con su firmware y su ficha.

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
| [`comun/`](comun/) | Trama de la mesh, `nodo_mesh.h` (la biblioteca de los nodos), cola persistente, `ingest_id`, radio ESP-NOW, y sus tests de host | cátedra |
| [`infraestructura/`](infraestructura/) | `nodo_gateway` y `nodo_sala` de la mesh, con sus `config_local.h.example` | cátedra |
| [`dispositivos/`](dispositivos/) | **Una carpeta por dispositivo, con todo adentro** (firmware, ficha, bibliotecas, tests), nombrada con su código de ubicación (`E1-PB-LECA-HFR01`); y el mapa de códigos | cada grupo |
| [`ejemplos/`](ejemplos/) | `plantilla_dispositivo/` y `sensor_ejemplo/` | cátedra |
| [`herramientas/`](herramientas/) | `broker.sh`, `ingesta_falsa.py`, `leer_mac/`, `generar_autocontenidos.sh` | cátedra |
| [`autocontenido/`](autocontenido/) | Copia de cada sketch con los headers de `comun/` al lado, para abrir en el IDE. **Se genera, no se edita** | — |
| [`docs/`](docs/) | Copia publicada del contrato, ADRs, el diseño de la mesh v2 y el histórico de la v1 | cátedra |

## Estado

La mesh implementa la **versión 3.0 del contrato** (trama ESP-NOW v2):

- La telemetría entra a AURA por `POST /api/v1/telemetry/ingest`, de a un evento, y el gateway
  confirma la muestra al nodo **solo** si AURA dice que la persistió.
- Cada nodo guarda sus muestras en flash con su `ingest_id` antes de enviarlas, y las conserva
  hasta esa confirmación: un corte del gateway, del WiFi o de AURA no pierde datos, dentro de
  la capacidad de la cola (24 h a una muestra por minuto).
- `set_config` llega al nodo, que responde con el resultado y la configuración vigente
  (`aplicado` o `rechazado`). Hay reportes de estado, alertas de sonda una vez por cambio y
  offline inferido.

Un dispositivo de la mesh se arma sobre [`comun/nodo_mesh.h`](comun/nodo_mesh.h): ver
[`ejemplos/sensor_ejemplo/`](ejemplos/sensor_ejemplo/) y la plantilla. El diseño está en
[`docs/mesh-v2/`](docs/mesh-v2/).

> ⚠️ El backend de AURA todavía **no acepta `aplicado`** y el endpoint REST tiene dos defectos
> documentados (contrato §6 y §10). El gateway ya los contempla, pero hasta que el backend se
> corrija, `aplicado` se descarta del lado de AURA.

Para el banco sin backend: [`herramientas/ingesta_falsa.py`](herramientas/ingesta_falsa.py)
hace de API, con modos para simular la caída y el defecto de `201` con errores.

## Sumar un dispositivo

Ver [`CONTRIBUTING.md`](CONTRIBUTING.md). En corto: cada dispositivo es una carpeta con todo
adentro, nombrada por dónde está y qué es (`E1-PB-LECA-HFR01`, con el mapa de
[`dispositivos/README.md`](dispositivos/README.md)). Se suma por fork y pull request. El CI
compila, corre los tests, valida los códigos y busca secretos.

## Compilar

Placa **XIAO_ESP32S3** con **USB CDC On Boot: Enabled** (sin eso el monitor serie queda mudo),
core `esp32:esp32` 3.3.11. Con `arduino-cli`:

```bash
arduino-cli compile --fqbn "esp32:esp32:XIAO_ESP32S3:CDCOnBoot=cdc" infraestructura/nodo_gateway
```

Los sketches incluyen `comun/` con rutas relativas (`../../comun/…`), y así compilan tanto en
`arduino-cli` como en el IDE 2. Si preferís abrir una carpeta suelta, usá la de `autocontenido/`.

Bibliotecas: las de cada `bibliotecas.txt` (ArduinoJson para todo lo que usa la mesh).
Cada sketch lee su configuración (MAC, red, UUID) de un `config_local.h` que no se versiona:
copiá el `config_local.h.example` de su carpeta. Sin él compila, pero no transmite.

Tests de host, sin placa:

```bash
make -C comun/tests
make -C infraestructura/nodo_gateway/tests
```

## Seguridad

**Este repositorio es público.** Credenciales de WiFi, claves de LoRaWAN (AppKey), tokens y
MAC de producción van en `config_local.h`, que está en el `.gitignore`. El CI rechaza cualquier
PR que los versione. Si algo se filtra igual, avisá a la cátedra: borrarlo en un commit nuevo
**no alcanza**, porque queda en la historia; hay que cambiar la clave.

`main` está protegida: los cambios entran solo por pull request.
