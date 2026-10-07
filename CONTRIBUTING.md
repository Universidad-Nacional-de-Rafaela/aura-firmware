# Cómo sumar un dispositivo

Guía para los grupos de Ingeniería en Computación III y IV.

## Una carpeta por dispositivo

Cada dispositivo instalado tiene **una carpeta con todo adentro** en [`dispositivos/`](dispositivos/):
su firmware, su ficha, sus bibliotecas y sus tests. La carpeta se llama con el **código del
dispositivo**, que dice dónde está y qué es:

```
E1-PB-LECA-HFR01        Edificio 1, planta baja, LabECA, heladera-freezer 01
```

Cada parte sale del mapa de [`dispositivos/README.md`](dispositivos/README.md). Ejemplo: en el
LabECA hay un sensor de temperatura (`E1-PB-LECA-TEM01`), uno de humedad (`E1-PB-LECA-HUM01`),
el consumo del tablero (`E1-PB-LECA-TAB01`), uno de luz (`E1-PB-LECA-LUZ01`) y el actuador de una
cortina (`E1-PB-LECA-CRT01`): cinco dispositivos, cinco carpetas.

Si un dispositivo nuevo usa el mismo firmware que otro (por ejemplo, otra heladera con el mismo
hardware), se copia esa carpeta con el código nuevo, y la ficha dice de cuál está basado.

## 1. Antes de escribir código

1. **Leé el contrato**: [`docs/CONTRATO_MQTT.md`](docs/CONTRATO_MQTT.md). Como mínimo §2.5 (un
   dispositivo es una placa), §3.1 (qué va en `values`), §3.3 (`set_config`) y §3.5 (alertas).
2. **Definí el transporte** con la cátedra: mesh ESP-WIFI-MESH si está en interior, LoRaWAN si
   está en exterior o lejos ([ADR-003](docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md),
   [ADR-006](docs/adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md)).
3. **Armá el código del dispositivo** con el mapa. Si te falta una abreviatura (un recinto
   nuevo, un tipo nuevo), la agregás a su tabla en el mismo PR.
4. **Pedí el alta del dispositivo** en AURA. Le pasás a la cátedra el `hw_id` de tu placa: si
   es mesh, el que imprime `herramientas/leer_mac` (`mac-…`); si es LoRaWAN, el DevEUI. La
   cátedra asigna esa placa a tu dispositivo en AURA, y te da los datos de la mesh del edificio
   (ID y clave) o las credenciales OTAA. Nada de eso se commitea. Mientras la placa no esté
   asignada, lo que mande queda en **cuarentena** en AURA y se recupera al asignarla.

## 2. Tu carpeta

1. Hacé un **fork** de este repositorio y trabajá en una rama.
2. Copiá [`ejemplos/plantilla_dispositivo/`](ejemplos/plantilla_dispositivo/) a
   `dispositivos/<CÓDIGO>/`, y renombrá el `.ino` igual que la carpeta
   (`E1-PB-LECA-HFR01.ino`): Arduino lo exige.
3. Completá el `README.md`, que es la ficha: ubicación, responsable, hardware, campos de
   `values`, alertas, parámetros de `set_config` y su valor en esta placa.
4. Listá en `bibliotecas.txt` cada biblioteca con su versión (`OneWire@2.3.8`). El CI instala
   exactamente esas.
5. Sumá el dispositivo a la tabla *Dispositivos* de `dispositivos/README.md`, y tu carpeta a
   `.github/CODEOWNERS` con los usuarios de GitHub del grupo.

**Si el dispositivo va por la mesh**, la plantilla ya viene armada sobre
[`comun/nodo_mesh.h`](comun/nodo_mesh.h). Tu placa es una **hoja**: no reenvía tráfico de otros.
Esa biblioteca se ocupa de unirse a la mesh, de la cola en flash, del
`ingest_id`, de reintentar hasta que AURA confirma, de `set_config` y de las alertas. A vos te
queda escribir la medición y dos funciones: `aplicar_config()`, que valida y guarda los
parámetros, y `describir_config()`, que devuelve la configuración vigente e incluye
`intervalo_s`. Si tu dispositivo acciona algo, agregás `ejecutar_comando()` para tus comandos
propios. Tu nodo no conoce su UUID ni la MAC del raíz: se identifica por la MAC de su placa.
[`ejemplos/sensor_ejemplo/`](ejemplos/sensor_ejemplo/) es el ejemplo completo más chico, y
[`ejemplos/actuador_ejemplo/`](ejemplos/actuador_ejemplo/) muestra un comando propio.

Para probar sin la mesh: sin `config_local.h` el sketch compila, mide y guarda en la cola, y el
monitor serie muestra cada medición. Con la mesh y sin AURA, la cátedra levanta el raíz y
[`herramientas/ack_falso.py`](herramientas/ack_falso.py), y con `herramientas/broker.sh datos`
ves lo que llega.

Si usás código de `comun/`, incluilo con ruta relativa (`#include "../../comun/protocolo_aura.h"`)
y corré `herramientas/generar_autocontenidos.sh` antes de commitear. Si tu lógica se puede probar
sin placa (formato de trama, validación de rangos, cola), poné los tests en
`dispositivos/<CÓDIGO>/tests/` con un `Makefile`: el CI los corre solo.

## 3. Lo que no se negocia

- **Ninguna credencial en el repo.** WiFi, clave de la mesh, AppKey, tokens, MAC de producción: todo en
  `config_local.h`, que está en el `.gitignore`. Este repo es **público**.
- **Ningún dato falso.** Si un sensor no responde, ese campo no se envía. No se reemplaza por
  otra lectura (por ejemplo, la temperatura interna del ESP32) ni por un valor fijo.
- **Configuración remota validada.** Cada parámetro de `set_config` se valida contra un rango
  antes de aplicarse, y se guarda en memoria no volátil. Un comando mal armado no puede dejar
  el nodo inutilizable.
- **No tocar `comun/` ni `infraestructura/` en el mismo PR que tu dispositivo.** Si necesitás
  un cambio ahí, abrí un issue o un PR aparte: afecta a todos los dispositivos.

## 4. Si el equipo se muda

Su carpeta se renombra con el código nuevo (también el `.ino`), y la ficha anota el anterior en
*Códigos anteriores*. En AURA conserva su `device_id`, así que el historial no se corta.

## 5. El pull request

Abrilo contra `main` de este repo. La plantilla del PR trae un checklist. El CI:

| Chequeo | Qué hace |
|---|---|
| Sin secretos | Rechaza `config_local.h` versionados y corre `gitleaks` sobre toda la historia |
| Códigos de dispositivos | Cada carpeta respeta el formato y el mapa, tiene su ficha y su `.ino` se llama igual |
| Tests de host | `comun/tests`, `infraestructura/*/tests`, `herramientas/tests` y los `tests/` de cada dispositivo |
| Compilar sketches | Compila cada sketch para XIAO ESP32S3 y verifica que `autocontenido/` esté al día |

Con todo en verde, la cátedra revisa y mergea.
