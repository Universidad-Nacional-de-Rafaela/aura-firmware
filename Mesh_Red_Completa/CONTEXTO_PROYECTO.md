# Contexto del Proyecto AURA (para retomar en Claude Code)

> Copia del `CLAUDE.md` local del proyecto (el repo ignora los `CLAUDE.md`). Las claves reales no están acá: van en `config_local.h` y `credenciales.h`.

## Qué es
**AURA**: sistema IoT para el edificio inteligente de la UNRaf (Ingeniería en Computación). Firmware en **ESP-IDF v6.1.0** con VS Code (extensión Espressif ESP-IDF 2.3.0), sobre Windows.

## Entorno
- ESP-IDF: `C:\esp\v6.1\esp-idf`, instalado con EIM (Easy Installation).
- Herramientas: `C:\Espressif\tools` (GCC 15.2.0, CMake, Ninja, Python 3.14 venv en `C:\Espressif\tools\python\v6.1\venv`).
- Variable de entorno de usuario: `PYTHONUTF8=1`.
- No hacen falta las extensiones ESP-IDF WEB ni ESP LowCode: Matter.
- Los proyectos van en carpetas locales sin espacios ni tildes y fuera de Google Drive u OneDrive.

## Arquitectura decidida
- **ESP-WIFI-MESH sin router**, con **raíz fija** (`esp_mesh_fix_root(true)`, sin elección de raíz).
- Nodos **ESP32-C3** por aula, con sensores y actuadores.
- Concentrador **ESP32-S3** con pantalla por aula, como comando local. Es un padre fijo con un plan B.
- Nodos de piso ubicados estratégicamente, que se auto-organizan libremente.
- Una única raíz para todo el edificio, conectada por **Ethernet** (no WiFi, para aislarla del AP saturado).
- Los dispositivos fuera del edificio se conectan por **LoRa → gateway Ethernet**.
- **Mensajes**: JSON dentro de la malla. La raíz publica por **MQTT** con un formato inspirado en **Sparkplug B** (ISO/IEC 20237:2023):
  - Edge Node = raíz; Devices = nodos.
  - Tópicos: `spBv1.0/{grupo}/{tipo}/{edge_node}/{device_id}`.
  - Primero un BIRTH con la definición de métricas y sus alias; después, DATA solo con alias y valor.
- **A futuro**:
  - BACnet/IP y KNXnet/IP, expuestos en el servidor central (no en los nodos).
  - Huella RS-485 en los nodos para Modbus RTU y BACnet MS/TP.
  - OTA, a implementar manualmente.

## Proyectos de prueba (ambos FUNCIONANDO)
| | Raíz / concentrador | Nodo hijo |
|---|---|---|
| Carpeta | `F:\Proyecto_AURA\Prueba_MESH_IDF\` | `F:\Proyecto_AURA\Prueba_MESH_NODO\` |
| Nombre (`project()`) | `mesh_raiz` | `mesh_nodo` |
| Placa | ESP32-S3-WROOM-2 (flash octal 16/32 MB) | ESP32-C3 SuperMini (4 MB) |
| Target | esp32s3 | esp32c3 |
| Puerto en la PC de Milton | COM7 | COM9 |
| Flash method | UART | UART |
| menuconfig | Flash size del módulo + Enable Octal Flash; Partition Table: Single factory app (large) | Flash 4 MB; Partition Table: Single factory app (large) |
| MAC | 90:e5:b1:d6:39:7c (AP: …:7d) | 90:70:69:ad:d9:64 |

Estructura de cada proyecto:
```
CMakeLists.txt        → cmake_minimum_required(VERSION 3.16)
                        include($ENV{IDF_PATH}/tools/cmake/project.cmake)
                        project(<nombre>)
main/CMakeLists.txt   → idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
                          PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash)
main/main.c
```

### Qué hace cada firmware (ver el código en `main/main.c` de cada carpeta)
- **Raíz**:
  - `esp_mesh_set_type(MESH_ROOT)`, `fix_root(true)`, `set_self_organized(false,false)`.
  - Usa un SSID de router ficticio ("sin-router").
  - La tarea `mesh_rx` recibe con `esp_mesh_recv` y hace printf de `[MAC] json`.
  - Loguea las conexiones y desconexiones de hijos y los cambios en la tabla de ruteo.
- **Nodo**:
  - Es `MESH_LEAF` (`#define NODO_ES_HOJA 1`) y usa `fix_root(true)`.
  - La tarea `mesh_tx` envía a la raíz (`esp_mesh_send(NULL, …)`), cada 2 s, el JSON `{"n","capa","temp","hum","pres"}` con datos simulados.
- **Parámetros comunes** (deben ser IDÉNTICOS en todos los nodos):
  ```c
  #define MESH_CHANNEL   6
  #define MESH_AP_PASS   "CAMBIAR"        // la real: en config_local.h, la da la cátedra
  #define MESH_MAX_HIJOS 6
  #define MESH_MAX_CAPAS 6
  static const uint8_t MESH_ID[6] = {0,0,0,0,0,0}; // el real: en config_local.h
  ```
- **Resultado verificado**: el nodo C3 se une a la raíz S3 en la capa 2 y la raíz imprime los JSON recibidos.

## Mesh de AURA en ESP-IDF, contrato MQTT v4.0 (oct 2026): `Mesh3_Gateway`, `Mesh3_Intermedio`, `Mesh3_Hoja`, `componentes/`, `Broker_MQTT`
- **Decisión (2026-10-20):** esta es la BASE de la mesh de AURA; todo pasa a **ESP-IDF nativo** (la versión Arduino de `main` de `aura-firmware` se reemplaza). Las hojas de los grupos se programan sobre la biblioteca `componentes/nodo_mesh`.
- **FUNCIONANDO** de punta a punta con el contrato v4.0 (árbol `hw/<hw_id>/...`). Detalle y resultados en `Broker_MQTT/LEEME.md`; reglas para hojas en `GUIA_NODOS_HOJA.md` (mantenerla al día).
- `componentes/aura_mesh` (solo encabezados): trama binaria v3 igual a `comun/protocolo_aura.h` de Arduino + tipos internos **HORA** (9) y **LOTE** (10); utilidades (ingest_id, hw_id, tópicos, ISO, vendor IE). `componentes/nodo_mesh`: biblioteca de la hoja (padre por aula, cola en flash, confirmación, comandos, hora).
- Placas S3: **Waveshare ESP32-S3-DEV-KIT-NxRx** (LED RGB WS2812 en GPIO38, orden R-G-B; BOOT = GPIO0). Gateway = módulo WROOM-2 (COM12, octal 32 MB, `mac-90e5b1d64a94`); concentrador = WROOM-1 (COM11, quad 16 MB, `mac-dcb4d92a5878`). Hoja C3 SuperMini (COM9, `mac-907069add964`), LED GPIO8 activo en bajo.
- **No apoyar las placas una contra otra:** con raíz y concentrador tocándose hubo `auth expire` en bucle y reconexiones cada ~11 s (reason 209). Separadas, todo anduvo.
- **Identidad = MAC de la placa** (`hw_id = mac-...`, ADR-005). Sin MACs, ids ni aulas en ningún firmware: todas las hojas de un tipo llevan el mismo binario. El **aula** va en NVS y la asigna AURA con `set_config {"aula":"..."}` (hoja y concentrador). Placa nueva = entra sin aula (directo a la raíz), AURA la ve pendiente (cuarentena) y al aceptarla le asigna el aula. Decisión de Milton: por ahora desde AURA; el emparejamiento por botón del concentrador queda para más adelante.
- Gateway: raíz fija + WiFi a la primera red al alcance de la lista `REDES_WIFI` de `credenciales.h` (hoy la red de banco, canal 11, y después la de la facultad; si queda 5 min sin router se reinicia y prueba la siguiente). Adaptador del contrato: `data` (values, ingest_id, ts ISO, adaptador), `status` retenido, `response` (transmitido/recibido/aplicado/rechazado, 30 s sin resultado → `sin_resultado_del_nodo`), `alerts`, `ack` → CONFIRMACIÓN, offline por tabla de ruteo (`fuera_de_la_malla`, 20 s) o 3 × intervalo (`sin_tramas`), nodos conocidos en NVS (`gw_nodos`, marca offline a los que no vuelven tras reiniciarse), status propio cada 60 s con LWT y keepalive 30 s, client_id = su hw_id, sesión persistente, `MQTT_USUARIO`/`MQTT_CLAVE` opcionales. Publica con `esp_mqtt_client_publish` (con `enqueue` cada mensaje se demoraba hasta 1 s) y atiende la bajada en otra tarea; ida y vuelta de un comando 29–51 ms.
- Concentrador (`Mesh3_Intermedio`): aula en NVS; con aula la anuncia en el beacon (vendor IE OUI 02:A5:52). Recibe las tramas de sus hojas (modo aula), las muestra ("pantalla" = LED + serie; solo las hojas que informan su misma aula), `filtrar()` (hoy deja pasar todo; lugar para reglas locales) y las reenvía **una vez** en un LOTE (junta 50 ms) con la MAC de cada hoja. Contesta los pedidos de HORA. Al cambiarle el aula suelta a sus hojas. Botón BOOT = comando local `led` invertir a la hoja. **Aula sin red arriba:** la hoja le manda `ESTADO` (tipo 13, último valor; no sube a la raíz) cuando hay cola atrás y cada vez que se cuelga de su concentrador; el concentrador muestra solo lo último (ignora lo más viejo que lo mostrado). Probado con AURA caída, gateway apagado y concentrador que llega tarde. **Modo isla:** con 60 s sin raíz el concentrador se reinicia como raíz de la isla de su aula (último canal, guardado en NVS; flag en RTC), porque sin raíz ESP-WIFI-MESH no acepta conexiones nuevas; busca al gateway cada 30 s y vuelve solo. Probado: hoja reiniciada se vuelve a colgar en ~10 s; botón con gateway apagado, 20 de 20; salida de la isla en 13 s; los cambios llegan a AURA en orden. **Hoja ausente:** desconectada >20 s o 3 intervalos sin datos → LED rojo y aviso por serie; las hojas del aula se recuerdan en NVS (también tras reiniciar el concentrador).
- Hoja: **modo aula** (padre = concentrador que anuncia su aula), **plan B** (raíz u otro concentrador, nunca otra hoja; 5 s + 3 escaneos), **sin_aula** (directo a la raíz). En modo aula revisa cada 30 s que su padre siga siendo de su aula; para cambiar de padre usa `set_parent` sin desconectarse (si se desconecta, la pila la vuelve a colgar del mismo padre en 0,2 s).
- **Entrega garantizada (decisión de Milton: híbrido):** cola en RAM de 400 mediciones (~75 KB); lo que pasa 5 min sin confirmar se respalda en la partición `cola` de la flash (256 KB, registro circular; confirmar = 1 byte 0xFF→0x00 sin borrar) y se recupera al arrancar. En marcha normal no se escribe la flash. Probado: 15 respaldadas, recuperadas tras reiniciar la hoja y entregadas con su hora original. `ingest_id` UUID, sale solo con la CONFIRMACIÓN; reintentos 15 s…5 min con azar. **Eventos** (resultado, reporte, alerta) confirmados por la raíz con `EVENTO_OK` (tipo 11) recién al publicarlos; la hoja los reintenta. Al volver el broker la raíz manda `REPORTAR` (tipo 12) a todos: reportan su estado y la hoja manda lo pendiente ya (probado). **Hora:** SNTP en el gateway; la hoja la pide (HORA) y la ajusta con cada confirmación; lo medido sin hora se fecha con el uptime. Sin RTC con pila no hay hora tras un corte (DS3231 para los nodos que lo necesiten).
- **MESH_LEAF:** en IDF 6.1 `set_parent(MESH_LEAF)` da ESP_FAIL al instante (probado de nuevo el 2026-10-20 con la radio sana) y cambiar a LEAF ya conectada desconecta en bucle. La hoja intenta LEAF una vez y si no usa MESH_NODE con beacon de su AP a 60 000 TU y rechazando hijos.
- **País AR** (canales 1–13) en los tres. **Solo el gateway depende del WiFi** (`ROUTER_FICTICIO "aura-sin-router"` en concentrador y hoja).
- **Descubrimiento del broker:** `BROKER_URI ""` → primero el último broker que anduvo en esa red (NVS), si falla 3 veces broadcast UDP 18830 (`broker.py` contesta). La PC tiene que estar en la misma red, *Privada* en Windows.
- **Vigilante de envío** (`esp_mesh_send_block_time(5000)` + reconexión forzada tras 3 fallos) en hoja y concentrador.
- **Secretos fuera del código** (repo público `aura-firmware`): `MESH_ID_BYTES` y `MESH_AP_PASS` en `main/config_local.h` de cada proyecto (en `.gitignore`; plantilla `config_local.ejemplo.h`); el gateway tiene además `credenciales.h` (plantilla `credenciales.ejemplo.h`). Publicado en `aura-firmware`, rama `feature/mesh-red-completa-idf`, carpeta `Mesh_Red_Completa/`.
- `Broker_MQTT/broker.py`: broker de banco con retenidos, LWT, keepalive y **AURA falsa** (ack; modo ok/sin_ack/cuarentena/rechazado/apagada) + tablero por `hw_id` en http://localhost:8080 (LED, asignar aula, intervalo). `led_blink.py <hw_id>`.
- Decisiones pendientes: prioridades y reglas locales del concentrador (Milton avisa); Sparkplug B para después.
- **Actualización por la malla (OTA, rama `feature/mesh-ota`, PR #24 que depende del #23; ADR-008, contrato 4.3 propuesta):** `componentes/aura_ota` (receptor de hoja y concentrador, tipo/versión, vuelta atrás) + `Mesh3_Gateway/main/ota_gateway.c` + `Broker_MQTT/ota.py`. **Tipo** = `project(aura-<uso>)` (uno por uso: `aura-gateway`, `aura-concentrador`, `aura-hoja-ejemplo`) y **versión** = `set(PROJECT_VER "x.y.z")`, grabados en la imagen; cada placa los informa en `status.details.firmware`. Comando `ota` (`params.url`, `sha256`, `forzar_tipo`) a cualquier `hw_id`, incluido el gateway; el gateway descarga una vez (partición `imagen` 4 MB, cache por SHA-256), reparte **una placa por vez** (ventanas de 4 × 1400 B que pide el nodo; 1 MB en ~10 s) y se actualiza último (15 s de gracia). Controles: chip, tipo, misma versión → `aplicado` `sin_cambios`, SHA-256, imagen válida. **Vuelta atrás:** la versión nueva queda si habló con el gateway (trama OTA `CONFIRMADA`, sin depender del broker) **y** anduvo 60 s; si no, en 10 min o al reiniciarse antes, vuelve sola a la anterior (`rechazado` `volvio_a_la_version_anterior`); antes respalda la cola. Particiones: `ota_0`/`ota_1` (hoja 1,75 MB c/u + `cola`; S3 3 MB c/u) + `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. **La primera vez, USB.** `python ota.py imagen.bin --hw mac-…` o `--todas [--aula aulaX]`; reintenta lo que no terminó si el gateway se reinicia o se corta el broker. Pruebas y resultados: `PRUEBAS_OTA_Y_CORTES.md`.
- **Reconexión (2026-10-22):** el concentrador, al perder el padre, lo elige de nuevo (`esp_mesh_set_self_organized(true,true)`): tras reiniciar el gateway vuelve en ~2 s (antes, bucle de `handshake timeout` hasta isla, >60 s). PMF apagado en el AP de la malla (en `MESH_EVENT_STARTED`). La hoja guarda en NVS el canal del último padre y lo escanea primero: vuelve en ~4 s tras reiniciarse. `seq` al azar al arrancar en los tres. Margen de 60 s al arrancar también para `sin_tramas`.
- **Prueba larga (2026-10-22, 6 h 11 min sin tocar nada):** 0 reinicios, 0 desconexiones; 2232 mediciones, 2232 confirmadas, 2232 `ingest_id` distintos; cola máxima 1.
- Compilar: `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` y `idf.py -p COMx build flash`. La C3 no se reinicia por RTS: usar `esptool.py --port COM9 --after hard-reset read-mac`.

## Errores resueltos (y solución)
| Síntoma | Causa | Solución |
|---|---|---|
| Errores de codificación en Python | Consola sin UTF-8 | `PYTHONUTF8=1` |
| Rutas o compilación fallan | Proyecto en Google Drive (`U:\Mi unidad`), OneDrive o una ruta con espacios o tildes | Mover el proyecto a `F:\` o `C:\` local |
| CMake no reconoce el comando | `proyect(...)` en lugar de `project(...)` | Corregir a `project` |
| Compila con Clang en vez de GCC | `CMakeLists.txt` raíz vacío o sin guardar al hacer Set Target | Guardar, borrar `build`, repetir Set Target |
| menuconfig o build fallan | Los dos `CMakeLists.txt` intercambiados | Raíz = `project()`; `main` = `idf_component_register()` |
| `out of memory` / muere `cc1.exe` | RAM ocupada por Docker o WSL | `wsl --shutdown`, cerrar Docker, o `ninja -C build -j 2` |
| El binario no entra en la partición | Partición por defecto de 1 MB | Single factory app (large) |
| `undefined reference to 'app_main'` | `build` configurado cuando `main/CMakeLists.txt` estaba mal; `main` no aparece en `-- Components:` | `Remove-Item -Recurse -Force build` y recompilar |
| Full Clean da error | El monitor o una terminal usa archivos | Cerrar el monitor (`Ctrl+]`) o borrar `build` a mano |
| La C3 SuperMini no engancha al padre | Antena chica | Al principio se usó `esp_wifi_set_max_tx_power(40)` (10 dBm). Eso **limita el alcance a ~3 m** con una pared; con la **antena de alambre de 31 mm** y `POTENCIA_TX 60` (15 dBm) llega a ~10 m a través de una vidriera, también con powerbank. Con antena + 19,5 dBm dejó de andar con cargador o powerbank (sí por USB de la PC): no subir la potencia con la antena puesta |
| La hoja no busca otro padre cuando cae el suyo (`STA is connecting, scan are not allowed!`) | Con auto-organización apagada, la pila reintenta el padre sin fin | `esp_mesh_set_self_organized(false,false)` corta los reintentos; después escanear |
| Tras reiniciar la raíz o un padre, el hijo no envía nada por minutos (`[WND-RX] … timeout`) | Control de flujo de la malla trabado con secuencia vieja | `esp_mesh_send_block_time(5000)` + `esp_mesh_disconnect()` tras 3 envíos fallidos |
| `esp_mesh_set_parent` devuelve ESP_FAIL | IDF 6.1 rechaza `MESH_LEAF` en set_parent | Usar `MESH_NODE` con beacon del AP a 60 000 TU y rechazando hijos |
| `auth expire` en bucle; un hijo entra y sale cada ~11 s (reason 209) | Placas tocándose | Separarlas |
| La hoja vuelve al mismo padre al desconectarse a propósito | Con set_parent la pila reintenta el último padre | Cambiar de padre con `set_parent` sin desconectarse |
| El ESP32 no ve el WiFi de la PC | La PC está en 5 GHz | El router también emite en 2,4 GHz; el gateway escanea y fija ese BSSID |
| El LED RGB de la placa S3 no prende | Waveshare ESP32-S3-DEV-KIT: el WS2812 está en GPIO38 (no 48) y usa orden R-G-B | `PIN_LED_RGB 38` y `LED_STRIP_COLOR_COMPONENT_FMT_RGB` |
| El gateway no llega al broker de la PC (timeout) | Regla del firewall de Windows que bloquea python.exe (Privada) | Permitir Python en el firewall en redes privadas (ya hecho en la PC de Milton) |
| El reporte de una placa recién reiniciada no se publica (AURA queda con el status viejo) | `seq` arrancaba en 0 y coincidía con la del último evento de antes: la raíz lo tomaba por repetido | `seq` al azar al arrancar |
| Tras reiniciar el gateway, el concentrador entra y sale cada ~10 s (`handshake timeout`, reason 204/15) hasta pasar a isla | La pila reintenta el padre de antes y el saludo WPA falla | El concentrador elige padre de nuevo al perderlo (`esp_mesh_set_self_organized(true,true)`); PMF apagado en el AP de la malla |
| Una versión nueva que se cuelga a los pocos segundos queda confirmada y la placa se reinicia en bucle | Se confirmaba apenas hablaba con la raíz | Confirmar solo si además anda 60 s (`AURA_FW_ESTABLE_S`); recuperar por USB |

**Regla general**: corregir el archivo, guardarlo (Auto Save activado), borrar `build` y recompilar.

## Documentación generada
- Guía paso a paso (entorno, creación del proyecto, configuración, build/flash/monitor, tabla de errores y checklist) en Google Docs: https://docs.google.com/document/d/1Do-hS3lmHAnvwI94Fp_e9jW_9CfkiOE6Oma4E44D7vk/edit
  - En la sección 3, los puertos ya están generalizados ("el COM donde aparece la placa en tu PC").
  - **Pendiente opcional**: generalizar también COM7/COM9 en la tabla "Placas usadas en la prueba" y en los títulos "Salida esperada… (COM7/COM9)".

## Próximos pasos posibles
- Guardar y reenviar: HECHO (cola en flash + confirmación de AURA). Botón local probado (solo actúa sobre hojas de su aula). Prueba larga de 6 h hecha (2026-10-22); falta una de días y con varias hojas.
- Formato Sparkplug B: queda para después (hoy contrato v4.0).
- Prioridades y reglas locales: Milton avisa cuando estén definidas.
- Raíz con Ethernet + cliente MQTT, publicando en formato Sparkplug B (BIRTH/DATA con alias).
- Concentrador S3 por aula como padre fijo con plan B, y nodos de piso auto-organizados.
- Sensores y actuadores reales en el nodo C3, en lugar de los datos simulados.
- Gateway LoRa → Ethernet para los dispositivos externos.
- OTA: HECHA por la malla (PR #24). Falta: que AURA la implemente (contrato, pendiente 22) e imágenes firmadas (pendiente 23).
