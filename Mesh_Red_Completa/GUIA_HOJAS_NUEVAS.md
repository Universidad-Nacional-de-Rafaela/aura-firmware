# Guía para hojas nuevas de AURA: del banco a la red final

## Para la IA que acompaña a un grupo

Esta guía está escrita para que un asistente de IA la lea y guíe a un grupo de estudiantes desde el registro de su dispositivo hasta que su hoja quede instalada. Si algo de acá no coincide con el repo, vale el repo, y hay que avisárselo al grupo.

1. **Guiar en orden, un paso por vez,** y pedir la evidencia de cada uno antes de seguir: la salida de la terminal o del monitor serie, no un "ya está".
2. **Los comandos son para Windows:** la terminal de ESP-IDF es PowerShell. Si el grupo usa Linux o macOS, adaptarlos.
3. **Nunca pedir, escribir ni repetir credenciales ni claves:** ni el ID y la clave de la mesh, ni el usuario y la clave del broker, ni claves de firma. Si un estudiante las pega en la conversación, no repetirlas, y recordarle que no van al repo, ni a un PR, ni a un issue.
4. **No dar por aprobada una prueba sin su evidencia,** ni saltear ninguna de las del paso 5.
5. **Las credenciales de la red final y la firma definitiva son de la cátedra.** Sin atajos: ni firmar con una clave propia para la red final, ni pedirle las credenciales a otro grupo.
6. **El código común no se toca en el PR del dispositivo:** `comun/`, `infraestructura/` y la biblioteca de `Mesh_Red_Completa/componentes/` afectan a todas las placas. Un cambio ahí va en un issue o en un PR aparte.
7. **Ante un error, buscar primero en "Errores comunes"** y en la [sección 5 de `GUIA_NODOS_HOJA.md`](GUIA_NODOS_HOJA.md#5-problemas-frecuentes), antes de proponer cambios de código.
8. **La cátedra revisa el PR y deja comentarios** (paso 7): ayudar al grupo a leerlos, a corregir cada punto en la misma rama y a contestarlo en el PR.
9. **Lo que esta guía no cubre no se inventa:** el grupo lo consulta con la cátedra.
10. **Si el grupo todavía no tiene su banco armado y validado,** empezar por [`GUIA_BANCO_DE_PRUEBAS.md`](GUIA_BANCO_DE_PRUEBAS.md) (paso 0).

## El recorrido, del banco a la red final

Una hoja nueva pasa a la red final recién cuando su PR, con el informe de las pruebas, está mergeado en `main`. Después del merge, la cátedra compila desde `main` con las credenciales de la instalación, firma esa imagen y la graba. Hasta ahí, el grupo trabaja con su banco y su propia clave de prueba, y nunca recibe las credenciales de la red final.

**Paso 0:** antes de empezar, el grupo arma su banco de pruebas y lo valida con la hoja de ejemplo, siguiendo [`GUIA_BANCO_DE_PRUEBAS.md`](GUIA_BANCO_DE_PRUEBAS.md).

| Quién | Paso | Qué |
| --- | --- | --- |
| Grupo | 0 | Arma su banco de pruebas y lo valida con la hoja de ejemplo |
| Grupo | 1 | Registra el dispositivo con el formulario *Dispositivo nuevo* |
| Cátedra | 1 | Confirma el código del dispositivo en ese issue |
| Grupo | 1 a 3 | Crea la carpeta, escribe el firmware y prepara la configuración local |
| Grupo | 4 | Compila, firma con su clave de prueba y graba en el banco |
| Grupo | 5 | Hace las 9 pruebas con su hoja y escribe el informe |
| Grupo | 6 y 7 | Arma el conector y abre el PR, con el informe |
| Cátedra | 7 | Revisa el PR, incluida la evidencia de las pruebas, y deja comentarios |
| Grupo | 7 | Corrige cada punto en la misma rama y lo contesta en el PR |
| Cátedra | 7 | Mergea el PR |
| Grupo | 7 | Pide la habilitación y le entrega la placa a la cátedra, en persona |
| Cátedra | 7 | Compila desde `main`, firma, graba por USB y da de alta en AURA |

La evidencia de las pruebas se revisa en el PR, antes del merge, y sin el PR mergeado la cátedra no compila ni firma. El avance de cada etapa queda registrado en el issue del dispositivo.

## Paso 1: registrar el dispositivo y crear su carpeta

Toda hoja nueva parte de la hoja de ejemplo de la cátedra, `Mesh_Red_Completa/Mesh3_Hoja/`, que ya trae la biblioteca, la OTA y la firma configuradas.

1. **Registrar el dispositivo, antes que nada:** un issue con el formulario *Dispositivo nuevo* (*Issues → New issue*). Ahí la cátedra confirma el código, y ahí se sigue el avance hasta la instalación.
2. **Primer PR, una vez por persona:** su archivo en `docs/contribuidores/`. Hasta tener un commit mergeado, el CI de sus PR no corre sin la aprobación manual de la cátedra.
3. **Fork y rama** de `Universidad-Nacional-de-Rafaela/aura-firmware`.
4. **El código del dispositivo** es el que confirmó la cátedra en el issue, según el mapa de `dispositivos/README.md` (por ejemplo, `E1-PB-LECA-TAB01`).
5. **Copiar `Mesh_Red_Completa/Mesh3_Hoja/` como `dispositivos/<CÓDIGO>/`**, en una ruta sin espacios ni tildes, y fuera de Google Drive u OneDrive.
6. **Ajustar el `CMakeLists.txt` de la carpeta:**
    - `set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../Mesh_Red_Completa/componentes")`
    - `set(PROJECT_VER "1.0.0")`: la versión, que se sube en cada entrega.
    - `project(aura-<uso>)`: el tipo de firmware, por ejemplo `aura-energia-pm2130`. Va por uso, no por instalación ([`GUIA_NODOS_HOJA.md`, 2.7](GUIA_NODOS_HOJA.md#27-tipo-y-versión-del-firmware)).
7. **Completar la ficha** (`README.md` de la carpeta): ubicación, responsables, hardware y conexionado, campos de `values`, alertas y parámetros de `set_config` con su valor en esa placa.
8. **Sumar el dispositivo** a la tabla de `dispositivos/README.md`, y la carpeta a `.github/CODEOWNERS` con los usuarios de GitHub del grupo.

| Archivo | Qué tiene que tener |
| --- | --- |
| `CMakeLists.txt` | `EXTRA_COMPONENT_DIRS`, `PROJECT_VER` y `project(aura-<uso>)` |
| `sdkconfig.defaults` | El target y la flash de la placa, sin sacar las líneas de la OTA y la firma (paso 2) |
| `partitions.csv` | `otadata`, `ota_0`, `ota_1` y `cola`, igual que en `Mesh3_Hoja` |
| `main/main.c` | La lógica propia del dispositivo (paso 2) |
| `main/config_local.ejemplo.h` | La plantilla de los datos de la mesh. Se sube; el `config_local.h` no |
| `README.md` | La ficha completa |
| `tests/` con `Makefile` | Los tests que corren sin placa, si la lógica se puede probar así: el CI los corre solo |

## Paso 2: la lógica del firmware

El grupo escribe solo lo propio del dispositivo; la biblioteca `nodo_mesh` hace la unión a la malla, el aula, la cola, los reintentos, la hora, la OTA y la verificación de la firma. Las reglas viven en [`GUIA_NODOS_HOJA.md`](GUIA_NODOS_HOJA.md); esta guía es el recorrido. Antes de pasar al paso 3, verificar:

- [ ] **Lo mínimo** ([2.1](GUIA_NODOS_HOJA.md#21-lo-mínimo)): `nodo_mesh_iniciar(&cfg)` una vez, después de `nvs_flash_init()`; la medición en una tarea propia, cada `intervalo_s`, con `nodo_mesh_medicion(values)`; `aplicar_config`, `describir_config` y, si la placa acciona algo, `ejecutar_comando`.
- [ ] **Las reglas** ([2.2](GUIA_NODOS_HOJA.md#22-reglas)): ni MAC, ni ID, ni aula en el código; `aplicar_config` valida todo o no aplica nada, e ignora `aula`; `describir_config` incluye `intervalo_s`; `ejecutar_comando` devuelve `true` solo si ejecutó; nada de `esp_mesh_*` ni `esp_wifi_*`; liberar el JSON con `cJSON_Delete`; decimales como `double` redondeados; `values` hasta 160 bytes.
- [ ] **Qué va en `values`** ([2.3](GUIA_NODOS_HOJA.md#23-qué-va-en-values-contrato-31)): solo mediciones válidas, con nombres en minúsculas y la unidad (`temp_c`, `v_an_v`, `p_w`). Una sonda que falla no se manda: se informa con `nodo_mesh_sonda`.
- [ ] **El tipo y la versión** ([2.7](GUIA_NODOS_HOJA.md#27-tipo-y-versión-del-firmware)): `project(aura-<uso>)` y `PROJECT_VER`.
- [ ] **La OTA** ([2.8](GUIA_NODOS_HOJA.md#28-actualización-por-la-malla-ota)): `partitions.csv` igual que en `Mesh3_Hoja`, y `app_main` sin bloquear, porque la actualización llega mientras la placa mide.
- [ ] **La firma** ([2.9](GUIA_NODOS_HOJA.md#29-imágenes-firmadas)): estas líneas en `sdkconfig.defaults`, como las trae `Mesh3_Hoja`:

```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
# CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES is not set
```

La última línea es la que hace que la compilación **no** firme: sin ella, `idf.py build` intenta firmar con una clave que no existe, y falla. Si falta la verificación de firma, la compilación se frena con un `#error`. El firmware completo tiene que entrar en 1,75 MB: `idf.py build` lo informa al final ("Smallest app partition is…").

**Recomendado:** que un `intervalo_s` nuevo rija enseguida, midiendo en ese momento, en lugar de esperar lo que quedaba del intervalo anterior. Está resuelto así en `dispositivos/E1-PB-LECA-TAB01/main/main.c`.

## Paso 3: configuración local, el hook de secretos y lo que nunca se sube

En el banco se trabaja con dos cosas que no van al repo: los datos de mesh del banco y una clave de firma de prueba del grupo. El repo es público.

1. **Los datos de la mesh:** los que el grupo generó para su banco (paso 0). Lo más seguro es copiar el `config_local.h` del gateway del banco como `main/config_local.h`, en lugar de tipear: un carácter distinto y la placa no entra a la malla.
2. **El hook de secretos, una vez por PC:** revisa cada commit y cada push, y frena en la PC lo que parezca un secreto, antes de que llegue a GitHub (ver `herramientas/hook_secretos/README.md`):

    ```
    powershell -ExecutionPolicy Bypass -File herramientas\hook_secretos\instalar.ps1
    ```

3. **La clave de firma de prueba,** generada una vez por grupo y guardada fuera del repo, por ejemplo en `C:\aura-claves\`:

    ```
    python -m espsecure generate-signing-key --version 2 --scheme rsa3072 C:\aura-claves\firma_prueba.pem
    ```

4. **Una copia de esa clave en un pendrive,** nunca en la nube ni en el repo. Las placas grabadas con ella solo aceptan actualizaciones firmadas con ella: si se pierde, hay que volver a grabarlas por USB.

**Nunca se sube:** `config_local.h`, `credenciales.h` (el del gateway), ninguna clave (`*.pem`, `*.key`, `claves/`), ni MAC, IP o ruta de producción. El `.gitignore` de la raíz los excluye, el hook los frena en la PC, y el CI corre `gitleaks` sobre toda la historia.

## Paso 4: compilar, firmar y grabar en el banco

La compilación no firma: se compila, se firma aparte con la clave de prueba, se verifica, y recién después se graba. Todo desde la terminal de ESP-IDF (PowerShell), parados en `dispositivos\<CÓDIGO>`:

```
idf.py set-target esp32c3
idf.py build
python ..\..\Mesh_Red_Completa\Broker_MQTT\firmar.py --clave C:\aura-claves\firma_prueba.pem build\<tipo>.bin
idf.py -p COMx erase-flash      # solo en la primera grabación
idf.py -p COMx flash monitor
```

`<tipo>` es el nombre del `project(...)`, por ejemplo `aura-energia-pm2130`. `firmar.py` firma la imagen, verifica la firma y se niega a firmar dos veces la misma imagen.

- **`erase-flash` va solo en la primera grabación,** por USB. En las siguientes, borraría también el aula, y habría que volver a asignarla.
- **Firmar una sola vez** cada imagen: con `espsecure` directo, firmar dos veces la deforma sin aviso; `firmar.py` lo impide.
- **El aviso `App built but not signed` aparece siempre** al compilar, aunque la imagen ya esté firmada.
- **`idf.py flash` no recompila si no cambió nada,** y graba la imagen firmada. Si se cambió algo después de firmar, hay que compilar y firmar de nuevo.
- **Grabar siempre una imagen firmada:** una placa que arranca con una imagen sin firma no acepta ninguna OTA.
- **Si la placa tiene fuente propia,** desconectarla mientras está por USB: dos fuentes a la vez pueden meter tensión en el USB de la notebook.

## Paso 5: las pruebas obligatorias

Con estas 9 pruebas aprobadas y la evidencia en el repo, la hoja queda en condiciones de abrir el PR. Se hacen con la hoja propia, en el banco ya validado (paso 0), con `broker.py`, el tablero y `ota.py` (ver `Mesh_Red_Completa/Broker_MQTT/LEEME.md`).

| N.º | Prueba | Cómo | Resultado esperado |
| --- | --- | --- | --- |
| 1 | Alta y aula | Encender la placa recién grabada y asignarle el aula del concentrador desde el tablero, en minúsculas | Aparece pendiente, pasa a `modo: aula` y llegan los `data` con `ts`, confirmados (`persistido`) |
| 2 | `set_config` inválido | Mandar un parámetro fuera de rango junto con uno válido | `rechazado` con motivo, y la configuración queda igual |
| 3 | Plan B | Apagar el concentrador del aula | La placa pasa a `directo`, no pierde datos y vuelve sola a `aula` |
| 4 | OTA normal | `ota.py` con la imagen firmada y el `PROJECT_VER` subido | `aplicado`, y el tablero muestra la versión nueva |
| 5 | Misma versión | `ota.py` con la imagen que ya tiene | `aplicado` sin cambios, sin instalar nada |
| 6 | Imagen sin firma | `ota.py` con una imagen compilada con el `PROJECT_VER` subido y sin firmar | `rechazado` (`imagen_invalida`), y sigue con la versión anterior |
| 7 | Otra clave | `ota.py` con una imagen con el `PROJECT_VER` subido, firmada con una segunda clave de prueba | `rechazado` (`imagen_invalida`) |
| 8 | Vuelta atrás | `ota.py` con una imagen con el `PROJECT_VER` subido, compilada con otro `MESH_ID_BYTES` y firmada con la clave del banco: no puede volver a la malla | En 10 min como máximo vuelve sola a la anterior: `rechazado` (`volvio_a_la_version_anterior`), y recupera las mediciones que guardó |
| 9 | Ningún dato perdido | Comparar las líneas `Medición guardada` del monitor de la hoja con los `ingest_id` distintos que recibieron `persistido` en el `mensajes.log` del broker | Cada medición guardada tiene su `ingest_id` con `persistido`. Un `duplicado` por un reintento no es un error: es lo que pide el contrato (§5) |

En las pruebas 6, 7 y 8, la versión tiene que ser distinta de la que corre: con la misma versión, la placa contesta `aplicado` sin cambios antes de mirar la firma, y la prueba no muestra nada.

**Recomendada:** cortar la energía del gateway en medio de una OTA. La transferencia tiene que volver a empezar sola y terminar en `aplicado`.

**La evidencia** va en `dispositivos/<CÓDIGO>/pruebas/<AAAA-MM-DD>-<tema>/README.md`: el procedimiento, el resultado de cada caso con extractos de los registros que lo respaldan (sin MAC, IP ni rutas), y lo que no se pudo probar. Los dos informes de `dispositivos/E1-PB-LECA-TAB01/pruebas/` sirven de modelo.

## Paso 6: el conector de AURA

Sin conector, AURA no guarda lo que manda la placa: lo que no está declarado, o está fuera de rango, se descarta. El detalle está en `CONTRIBUTING.md`, sección 3.

1. **Copiar `ejemplos/plantilla_conector/` como `conectores/<CÓDIGO>/`**, con el mismo código que el dispositivo.
2. **En `conector.toml`:** `nombre` igual a la carpeta, los usuarios de GitHub del grupo, y un bloque `[campos.<nombre>]` por cada campo que manda el firmware, con su unidad, su rango físico, su `etiqueta` (hasta 40 caracteres) y su `rol` (`principal`, `auxiliar` u `oculto`).
3. **En `conector.py`:** `al_recibir_datos(self, msg, ctx)` decide qué guardar (`ctx.guardar_medicion`) y qué descartar, y emite las alertas.
4. **Tests con `ContextoDePrueba`,** que corren sin AURA ni placa:

    ```
    python -m unittest discover -s conectores\<CÓDIGO>\tests
    ```

    Conviene sumar mediciones reales del banco, como `tablero_real.jsonl` en `conectores/E1-PB-LECA-TAB01/tests/`.
5. **Probarlo como corre en AURA,** en un proceso aparte y con límites de memoria y tiempo:

    ```
    python herramientas\correr_conector.py conectores\<CÓDIGO> --archivo mensajes.jsonl --subproceso
    ```

    `mensajes.jsonl` tiene una línea JSON por mensaje, con el payload de `data` tal como lo publica el gateway: `{"values": {...}, "ingest_id": "...", "ts": "..."}`. El formato es el de `tablero_real.jsonl`.

Los nombres de los campos del `conector.toml` tienen que ser exactamente los de `values` en el firmware.

## Paso 7: entregar y pedir la habilitación

El recorrido es uno solo, y la evidencia de las pruebas se revisa en el PR, antes del merge. Después del merge, la cátedra compila, firma y graba: el grupo entrega el código, no un `.bin`, y no recibe las credenciales de la red final ([`GUIA_NODOS_HOJA.md`, 2.9](GUIA_NODOS_HOJA.md#29-imágenes-firmadas)).

1. **El grupo abre el PR contra `main`,** con el checklist de la plantilla completo: dispositivo, conector, ficha, tests e informe de pruebas.
2. **La cátedra revisa el PR y deja comentarios,** incluida la evidencia de las pruebas. Cada punto se corrige en la misma rama (el PR se actualiza solo) y se contesta en el PR. Puede haber varias rondas.
3. **La cátedra mergea el PR.**
4. **El grupo pide la habilitación y le entrega la placa a la cátedra, en persona.** El `hw_id` no va en el PR ni en el issue: la cátedra graba la placa por USB y lee la MAC ella misma.
5. **La cátedra compila, firma, graba y da de alta:**
    - compila desde `main`, con las credenciales de la instalación. No firma `.bin` compilados por otros, y la imagen no se publica, porque lleva las credenciales de la malla;
    - la firma con su clave, que nunca sale de la cátedra;
    - la graba por USB, con `erase-flash`. Desde ahí, la placa solo acepta actualizaciones firmadas con esa clave;
    - y la da de alta en AURA: con su primera medición, la placa aparece en «Placas sin asignar», y la cátedra la asigna al código del dispositivo y le asigna el aula, en minúsculas. Si es un concentrador, además lo autoriza.

**Después, cada actualización** sigue el mismo camino: el grupo sube los cambios por PR, con el `PROJECT_VER` aumentado, y la cátedra la compila desde `main`, la firma y la manda por OTA desde AURA.

## Errores comunes

Todos salieron en el banco de `E1-PB-LECA-TAB01`; los problemas de la malla en general están en la [sección 5 de `GUIA_NODOS_HOJA.md`](GUIA_NODOS_HOJA.md#5-problemas-frecuentes).

| Síntoma | Causa | Solución |
| --- | --- | --- |
| La placa no entra a la malla: no aparece `Hijo conectado` en el monitor del gateway o del concentrador (con aula, la hoja se cuelga del concentrador) | `config_local.h` con los valores de la plantilla, o con un carácter distinto | Copiar el `config_local.h` de una placa que funciona, y recompilar |
| Después de cambiar `config_local.h`, `idf.py build` hace 5 pasos y no recompila | `copy` conserva la fecha del archivo original, y la compilación decide por fecha | Borrar la carpeta `build` y compilar de nuevo |
| La compilación se frena con un `#error` sobre la firma | Un `sdkconfig` de antes de las imágenes firmadas | Borrar `sdkconfig` y `build`, y compilar de nuevo |
| `idf.py build` falla al intentar firmar | Falta `# CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES is not set` en `sdkconfig.defaults` | Agregar la línea (paso 2), borrar `sdkconfig` y compilar de nuevo |
| La OTA termina en `imagen_invalida` con la clave correcta | La imagen se firmó dos veces, no se firmó, o la placa se grabó con otra clave | Compilar, firmar una vez y verificar; si la placa tiene otra clave, grabarla por USB |
| Una OTA de prueba contesta `aplicado` sin cambios en vez de rechazarse | La imagen tiene la misma versión que la que corre | Subir el `PROJECT_VER`, compilar y firmar de nuevo |
| El aula queda `rechazado` (`aula_invalida`) | Tiene mayúsculas, u otros caracteres, o más de 15 | Solo `a-z`, `0-9`, `-` y `_`, hasta 15 caracteres: `e1-pb-leca`, no `E1-PB-LECA` |
| La placa arranca con un aula vieja, o el gateway muestra placas que ya no existen | Se grabó sin `erase-flash`: la NVS conserva lo anterior | `erase-flash` antes de la primera grabación |
| La hoja está viva pero aparece `offline`, con los `pendientes` subiendo | Después de un corte, espera hasta unos 7,5 min para reintentar | Esperar: se recupera sola. Reiniciarla acorta la espera |
| `ota.py` o `mandar_comando.py` se quedan esperando sin error | Un Mosquitto instalado como servicio de Windows ocupa el puerto 1883 | `--broker <IP de la notebook>`, o detener el servicio |

## Dónde está cada cosa en el repo

| Ruta | Qué tiene |
| --- | --- |
| [`GUIA_BANCO_DE_PRUEBAS.md`](GUIA_BANCO_DE_PRUEBAS.md) | Cómo arma y valida cada grupo su banco de pruebas: el paso 0 |
| [`GUIA_NODOS_HOJA.md`](GUIA_NODOS_HOJA.md) | Las reglas de una hoja: API de la biblioteca (2.1), reglas (2.2), OTA (2.8), firma (2.9), checklist (3) y problemas frecuentes (5) |
| [`CONTRIBUTING.md`](../CONTRIBUTING.md) | Cómo sumar un dispositivo: registro, carpeta, conector, lo que no se negocia y el PR |
| [`.github/ISSUE_TEMPLATE/dispositivo.yml`](../.github/ISSUE_TEMPLATE/dispositivo.yml) | El formulario *Dispositivo nuevo*, para registrar el dispositivo y seguir su avance |
| [`herramientas/hook_secretos/`](../herramientas/hook_secretos/) | El hook que revisa secretos en cada commit y cada push |
| [`docs/CONTRATO_MQTT.md`](../docs/CONTRATO_MQTT.md) | El contrato: qué va en `values` (3.1), `set_config` (3.3), alertas (3.5) y duplicados (5) |
| [`docs/adr/ADR-008-…`](../docs/adr/ADR-008-actualizacion-por-la-malla-y-tipo-de-firmware.md) | La OTA y el tipo de firmware |
| [`Mesh3_Hoja/`](Mesh3_Hoja/) | La hoja de ejemplo: la plantilla para copiar |
| [`componentes/nodo_mesh/`](componentes/nodo_mesh/) | La biblioteca de las hojas |
| [`Broker_MQTT/`](Broker_MQTT/) | El banco: `broker.py`, el tablero, `ota.py`, las herramientas (`firmar.py`, `pem_a_define.py`, `buscar_brokers.py`, `mandar_comando.py`) y su `LEEME.md` |
| [`ejemplos/plantilla_conector/`](../ejemplos/plantilla_conector/) | La plantilla del conector |
| [`dispositivos/E1-PB-LECA-TAB01/`](../dispositivos/E1-PB-LECA-TAB01/) | Una hoja completa de ejemplo: Modbus RTU por RS-485, tests sin placa e informes de prueba en `pruebas/` |
| [`conectores/E1-PB-LECA-TAB01/`](../conectores/E1-PB-LECA-TAB01/) | Un conector completo, con tests sobre mediciones reales |
