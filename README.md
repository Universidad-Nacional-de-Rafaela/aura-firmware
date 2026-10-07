# aura-firmware

Firmware de los dispositivos del proyecto **AURA** (Administración Unificada de Recursos y
Accesos), Universidad Nacional de Rafaela. Acá vive el código que corre **en las placas**: la
infraestructura de la mesh que mantiene la cátedra (raíz y relevos), y una carpeta por cada
dispositivo que desarrollan los grupos de Ingeniería en Computación III y IV, con su firmware y
su ficha.

La plataforma (backend y web) está en otro repositorio. Lo único que une a los dos es el
**contrato**: [`docs/CONTRATO_MQTT.md`](docs/CONTRATO_MQTT.md). Si un dispositivo no lo
respeta, AURA no falla: descarta el mensaje en silencio.

## Dos transportes

Según [ADR-003](docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) y
[ADR-006](docs/adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md):

- **Interior → mesh ESP-WIFI-MESH.** El dispositivo es una **hoja** de la mesh del edificio. La
  mesh arma sola el camino hasta el **raíz**, que es el único con WiFi y el que habla con AURA.
  Donde las hojas no llegan al raíz, la cátedra pone **relevos**.
  ```
  dispositivo (hoja) --mesh--> nodo_relevo --mesh--> nodo_raiz --WiFi/MQTT--> AURA
  ```
- **Exterior o lejos → LoRaWAN**, por el gateway LoRa y ChirpStack. El banco del aula y un
  nodo de ejemplo están en
  [`IC-lorawan-test`](https://github.com/Universidad-Nacional-de-Rafaela/IC-lorawan-test).

## Estructura

| Carpeta | Qué hay | Quién la mantiene |
|---|---|---|
| [`comun/`](comun/) | Trama de la mesh, `nodo_mesh.h` (la biblioteca de las hojas), cola persistente, `ingest_id`, radio ESP-WIFI-MESH, y sus tests de host; `python/aura_sdk/`, la biblioteca de los conectores | cátedra |
| [`infraestructura/`](infraestructura/) | `nodo_raiz` y `nodo_relevo` de la mesh, con sus `config_local.h.example` | cátedra |
| [`conectores/`](conectores/) | **Un conector por dispositivo**: el código que procesa sus datos dentro de AURA (valida, guarda, alerta, predice), en Python | cada grupo |
| [`dispositivos/`](dispositivos/) | **Una carpeta por dispositivo, con todo adentro** (firmware, ficha, bibliotecas, tests), nombrada con su código de ubicación (`E1-PB-LECA-HFR01`); y el mapa de códigos | cada grupo |
| [`ejemplos/`](ejemplos/) | Firmware: `plantilla_dispositivo/`, `sensor_ejemplo/` (una medición) y `actuador_ejemplo/` (un comando). Conectores: `plantilla_conector/` y `conector_ejemplo/` | cátedra |
| [`herramientas/`](herramientas/) | `broker.sh`, `ack_falso.py`, `correr_conector.py`, `leer_mac/`, `generar_autocontenidos.sh`, validadores | cátedra |
| [`autocontenido/`](autocontenido/) | Copia de cada sketch con los headers de `comun/` al lado, para abrir en el IDE. **Se genera, no se edita** | — |
| [`docs/`](docs/) | Copia publicada del contrato, ADRs, el diseño de la mesh ([`mesh-wifi/`](docs/mesh-wifi/)) y el [histórico](docs/historico/) de la mesh ESP-NOW | cátedra |

## Estado

La mesh implementa la **versión 4.0 del contrato** sobre **ESP-WIFI-MESH** (trama v3).

> ⚠️ **Compilado y con tests de host, pero todavía sin probar en placa.** Lo que falta verificar
> está en [`docs/mesh-wifi/estado-de-banco.md`](docs/mesh-wifi/estado-de-banco.md). La versión
> anterior, sobre ESP-NOW y probada en placa, está en el tag `mesh-espnow-v2`.

- Cada dispositivo se identifica por la **MAC de su placa** (`hw_id` `mac-…`): no conoce su
  UUID en AURA ni la MAC del raíz. AURA asigna la placa a su dispositivo.
- La telemetría sale por el raíz a `hw/<hw_id>/data`. AURA contesta en `hw/<hw_id>/ack` cuando
  la guardó, y recién ahí el raíz se la confirma a la hoja.
- Cada hoja guarda sus muestras en flash con su `ingest_id` antes de enviarlas, y las conserva
  hasta esa confirmación: un corte del raíz, del WiFi o de AURA no pierde datos, dentro de la
  capacidad de la cola (24 h a una muestra por minuto).
- `set_config` y los comandos propios llegan a la hoja, que responde con el resultado y la
  configuración vigente (`aplicado` o `rechazado`). Hay reportes de estado, alertas de sonda una
  vez por cambio y offline inferido.

Un dispositivo de la mesh se arma sobre [`comun/nodo_mesh.h`](comun/nodo_mesh.h): ver
[`ejemplos/sensor_ejemplo/`](ejemplos/sensor_ejemplo/), [`ejemplos/actuador_ejemplo/`](ejemplos/actuador_ejemplo/)
y la plantilla. El diseño está en [`docs/mesh-wifi/`](docs/mesh-wifi/).

> ⚠️ El backend de AURA todavía escucha el árbol de la v3.0 y no publica el `ack` (contrato §10,
> pendientes 1 y 6). Para el banco sin backend, [`herramientas/ack_falso.py`](herramientas/ack_falso.py)
> hace de AURA, con modos para simular la base caída, `rechazado` y `cuarentena`.

## Sumar un dispositivo

Ver [`CONTRIBUTING.md`](CONTRIBUTING.md). En corto: cada dispositivo es una carpeta con su
firmware en `dispositivos/` y su conector en `conectores/`, con todo adentro, nombrada por dónde está y qué es (`E1-PB-LECA-HFR01`, con el mapa de
[`dispositivos/README.md`](dispositivos/README.md)). Se suma por fork y pull request. El CI
compila, corre los tests, valida los códigos y busca secretos.

## Compilar

Placa **XIAO_ESP32S3** con **USB CDC On Boot: Enabled** (sin eso el monitor serie queda mudo),
core `esp32:esp32` 3.3.11. Con `arduino-cli`:

```bash
arduino-cli compile --fqbn "esp32:esp32:XIAO_ESP32S3:CDCOnBoot=cdc" ejemplos/sensor_ejemplo
```

Los sketches incluyen `comun/` con rutas relativas (`../../comun/…`), y así compilan tanto en
`arduino-cli` como en el IDE 2. Si preferís abrir una carpeta suelta, usá la de `autocontenido/`.

Bibliotecas: las de cada `bibliotecas.txt` (ArduinoJson para todo lo que usa la mesh).
Cada sketch lee su configuración (mesh, red, MAC esperada) de un `config_local.h` que no se
versiona: copiá el `config_local.h.example` de su carpeta. Sin él compila; una hoja sin
configurar mide y guarda en su cola, pero no se une a la mesh.

Tests de host, sin placa:

```bash
make -C comun/tests
make -C infraestructura/nodo_raiz/tests
make -C herramientas/tests
```

## Seguridad

**Este repositorio es público.** Credenciales de WiFi, la clave de la mesh, claves de LoRaWAN
(AppKey), tokens y MAC de producción van en `config_local.h`, que está en el `.gitignore`. El CI
rechaza cualquier PR que los versione. Si algo se filtra igual, avisá a la cátedra: borrarlo en
un commit nuevo **no alcanza**, porque queda en la historia; hay que cambiar la clave.

`main` está protegida: los cambios entran solo por pull request.
