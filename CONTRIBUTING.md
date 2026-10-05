# Cómo agregar el firmware de un dispositivo

Guía para los grupos de Ingeniería en Computación III y IV.

## 1. Antes de escribir código

1. **Leé el contrato**: [`docs/CONTRATO_MQTT.md`](docs/CONTRATO_MQTT.md). Como mínimo §2.5 (un
   dispositivo es una placa), §3.1 (qué va en `values`), §3.3 (`set_config`) y §3.5 (alertas).
2. **Definí el transporte** con la cátedra: mesh ESP-NOW si está en interior, LoRaWAN si está
   en exterior o lejos ([ADR-003](docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md)).
3. **Pedí el alta del dispositivo** en AURA. La cátedra te da el `device_id` (un UUID) y, si es
   LoRaWAN, las credenciales OTAA. Nada de eso se commitea.

## 2. Tu carpeta

1. Hacé un **fork** de este repositorio y trabajá en una rama.
2. Copiá `dispositivos/plantilla_dispositivo/` a `dispositivos/<nombre>/`. El nombre va en
   minúsculas con guion bajo (`heladera_freezer`), y **el `.ino` se llama igual que la
   carpeta**, porque Arduino lo exige.
3. Completá el `README.md` de tu carpeta: es lo que la cátedra revisa primero.
4. Listá en `bibliotecas.txt` cada biblioteca con su versión (`OneWire@2.3.8`). El CI instala
   exactamente esas.
5. Agregá tu carpeta a `.github/CODEOWNERS` con los usuarios de GitHub del grupo.

Si usás código de `comun/`, incluilo con ruta relativa (`#include "../../comun/protocolo_aura.h"`)
y corré `herramientas/generar_autocontenidos.sh` antes de commitear.

Si tu lógica se puede probar sin placa (formato de trama, validación de rangos, cola), poné
los tests en `dispositivos/<nombre>/tests/` con un `Makefile`: el CI los corre solo.

## 3. Lo que no se negocia

- **Ninguna credencial en el repo.** WiFi, AppKey, tokens, MAC de producción: todo en
  `config_local.h`, que está en el `.gitignore`. Este repo es **público**.
- **Ningún dato falso.** Si un sensor no responde, ese campo no se envía. No se reemplaza por
  otra lectura (por ejemplo, la temperatura interna del ESP32) ni por un valor fijo.
- **Configuración remota validada.** Cada parámetro de `set_config` se valida contra un rango
  antes de aplicarse, y se guarda en memoria no volátil. Un comando mal armado no puede dejar
  el nodo inutilizable.
- **No tocar `comun/` ni `infraestructura/` en el mismo PR que tu dispositivo.** Si necesitás
  un cambio ahí, abrí un issue o un PR aparte: afecta a todos los dispositivos.

## 4. El pull request

Abrilo contra `main` de este repo. La plantilla del PR trae un checklist. El CI:

| Chequeo | Qué hace |
|---|---|
| Sin secretos | Rechaza `config_local.h` versionados y corre `gitleaks` sobre toda la historia |
| Tests de host | `comun/tests` y los `tests/` de cada dispositivo |
| Compilar sketches | Compila cada sketch para XIAO ESP32S3 y verifica que `autocontenido/` esté al día |

Con los tres en verde, la cátedra revisa y mergea.
