# Cómo sumar un firmware o un dispositivo

Guía para los grupos de Ingeniería en Computación III y IV.

## Dos cosas distintas

| | Qué es | Dónde | Cómo se nombra |
|---|---|---|---|
| **Firmware** | El código. Se escribe una vez y puede correr en muchas placas | [`firmware/`](firmware/) | `Fnnn_descripcion`: un número único, que no se reutiliza nunca, y una descripción corta |
| **Dispositivo** | Una placa instalada en un lugar concreto | [`dispositivos/`](dispositivos/) | Su código de ubicación: `E1-PB-LECA-HFR01` |

El código de un dispositivo dice **dónde está y qué es**, de un vistazo. Cada parte sale del
mapa de [`dispositivos/README.md`](dispositivos/README.md). Cada ficha dice qué firmware usa.

Ejemplo: en el LabECA hay un sensor de temperatura (`E1-PB-LECA-TEM01`), uno de humedad
(`E1-PB-LECA-HUM01`), el consumo del tablero (`E1-PB-LECA-TAB01`), uno de luz
(`E1-PB-LECA-LUZ01`) y el actuador de una cortina (`E1-PB-LECA-CRT01`). Son cinco dispositivos
con cinco firmwares distintos. Si otro laboratorio pone un sensor de luz igual, usa el mismo
firmware que `E1-PB-LECA-LUZ01`, con otro código de dispositivo.

## 1. Antes de escribir código

1. **Leé el contrato**: [`docs/CONTRATO_MQTT.md`](docs/CONTRATO_MQTT.md). Como mínimo §2.5 (un
   dispositivo es una placa), §3.1 (qué va en `values`), §3.3 (`set_config`) y §3.5 (alertas).
2. **Definí el transporte** con la cátedra: mesh ESP-NOW si está en interior, LoRaWAN si está
   en exterior o lejos ([ADR-003](docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md)).
3. **Fijate si ya existe un firmware** en [`firmware/README.md`](firmware/README.md) que haga lo
   que necesitás. **Extender antes que duplicar**: si tu hardware difiere en algo chico (otra
   pantalla, otro pin), agregá una opción de `config_local.h` a ese firmware. Un firmware nuevo,
   solo si el código es sustancialmente otro.
4. **Pedí el alta del dispositivo** en AURA. La cátedra te da el `device_id` (un UUID) y, si es
   LoRaWAN, las credenciales OTAA. Nada de eso se commitea.

## 2. Un firmware nuevo

1. Hacé un **fork** de este repositorio y trabajá en una rama.
2. Tomá el número que sigue en la tabla de [`firmware/README.md`](firmware/README.md) y agregá
   tu fila.
3. Copiá [`ejemplos/plantilla_firmware/`](ejemplos/plantilla_firmware/) a
   `firmware/Fnnn_descripcion/`, y renombrá el `.ino` igual que la carpeta (Arduino lo exige).
4. Completá el `README.md` del firmware: hardware, campos de `values`, alertas, parámetros de
   `set_config`.
5. Listá en `bibliotecas.txt` cada biblioteca con su versión (`OneWire@2.3.8`). El CI instala
   exactamente esas.
6. Agregá tu carpeta a `.github/CODEOWNERS` con los usuarios de GitHub del grupo.

Si usás código de `comun/`, incluilo con ruta relativa (`#include "../../comun/protocolo_aura.h"`)
y corré `herramientas/generar_autocontenidos.sh` antes de commitear. Si tu lógica se puede probar
sin placa (formato de trama, validación de rangos, cola), poné los tests en
`firmware/Fnnn_descripcion/tests/` con un `Makefile`: el CI los corre solo.

## 3. Un dispositivo nuevo

1. Armá su código con el mapa de [`dispositivos/README.md`](dispositivos/README.md):
   `EDIFICIO-PISO-RECINTO-TIPOnn`. Si te falta una abreviatura (un recinto nuevo, un tipo
   nuevo), agregala a su tabla en el mismo PR.
2. Copiá [`dispositivos/_plantilla/`](dispositivos/_plantilla/) a `dispositivos/<CÓDIGO>/` y
   completá la ficha: ubicación, firmware que usa, responsable, configuración no secreta.
3. Sumá el dispositivo a la tabla *Dispositivos* de `dispositivos/README.md`, y su código a la
   columna *Dispositivos que lo usan* del firmware en `firmware/README.md`.

## 4. Lo que no se negocia

- **Ninguna credencial en el repo.** WiFi, AppKey, tokens, MAC de producción: todo en
  `config_local.h`, que está en el `.gitignore`. Este repo es **público**.
- **Ningún dato falso.** Si un sensor no responde, ese campo no se envía. No se reemplaza por
  otra lectura (por ejemplo, la temperatura interna del ESP32) ni por un valor fijo.
- **Configuración remota validada.** Cada parámetro de `set_config` se valida contra un rango
  antes de aplicarse, y se guarda en memoria no volátil. Un comando mal armado no puede dejar
  el nodo inutilizable.
- **No tocar `comun/` ni `infraestructura/` en el mismo PR que tu firmware.** Si necesitás un
  cambio ahí, abrí un issue o un PR aparte: afecta a todos los dispositivos.

## 5. El pull request

Abrilo contra `main` de este repo. La plantilla del PR trae un checklist. El CI:

| Chequeo | Qué hace |
|---|---|
| Sin secretos | Rechaza `config_local.h` versionados y corre `gitleaks` sobre toda la historia |
| Códigos y registro | Cada código de dispositivo respeta el formato y el mapa; cada firmware tiene número registrado y único |
| Tests de host | `comun/tests` y los `tests/` de cada firmware |
| Compilar sketches | Compila cada sketch para XIAO ESP32S3 y verifica que `autocontenido/` esté al día |

Con todo en verde, la cátedra revisa y mergea.
