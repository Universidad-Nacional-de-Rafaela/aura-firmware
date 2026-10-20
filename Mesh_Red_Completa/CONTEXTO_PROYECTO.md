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

## Prueba de 3 niveles + MQTT (oct 2026): `Mesh3_Gateway`, `Mesh3_Intermedio`, `Mesh3_Hoja`, `Broker_MQTT`
- **FUNCIONANDO** de punta a punta (subida y bajada por MQTT, ida y vuelta de ~19 ms). Detalle en `Broker_MQTT/LEEME.md` (placas, puertos, MAC, tópicos, resultado).
- Placas S3: **Waveshare ESP32-S3-DEV-KIT-NxRx** (LED RGB WS2812 en GPIO38, orden R-G-B; BOOT = GPIO0). Gateway = módulo WROOM-2; concentrador = módulo WROOM-1.
- Gateway S3 N32R16V (COM12, octal 32 MB): raíz fija + WiFi a la red de `credenciales.h` (hoy IngComputacion, canal 1; antes Personal-419) con el canal de 2,4 GHz detectado por escaneo + `espressif/mqtt` (en IDF 6 MQTT es componente externo).
- Concentrador (carpeta `Mesh3_Intermedio`) S3 N16R8 (COM11, quad 16 MB): capa 2, canal 0; anuncia su aula en el beacon (vendor IE OUI 02:A5:52); "pantalla" = LED WS2812 GPIO38 (orden RGB) + serie; botón BOOT = mando local.
- Hoja C3 (COM9): LED GPIO8 (activo en bajo). **Modo aula**: padre = el concentrador que anuncia su aula; manda cada mensaje UNA vez al concentrador, que lo muestra y lo reenvía a la raíz. **Plan B**: si no ve a su concentrador (5 s de margen + 3 escaneos), se cuelga de la raíz u otro concentrador (nunca de otra hoja) y manda directo; cada 30 s revisa si volvió (escaneo del canal actual). Probado.
- **Sin MACs en ningún lado**: cada nodo tiene `AULA`/`DEVICE_ID` en el firmware y manda `"id"`; el gateway aprende `id → ruta` del último mensaje. Reemplazar una placa = grabar el mismo firmware (probado).
- Tópicos: el nodo manda `"topic"` en el JSON; el gateway publica sin ese campo y agrega `"via"` (MAC de quien se lo entregó). Bajada: `aura/cmd/<id>`; el gateway agrega `"dest":"<id>"` y el concentrador lo pasa a la hoja. El resto del JSON pasa intacto (lugar previsto para un futuro `"prio"`).
- **Hora:** el gateway la toma por SNTP y la reparte cada 60 s (`{"dest":"<id>","hora":<ms UTC>}`); los nodos ponen `"ts"` (ms UTC) en sus mensajes, y si falta, el gateway pone la suya con `"ts_gw":true`. El ESP32 no tiene VBAT para el RTC: conservar la hora tras un corte requiere un RTC externo (DS3231).
- **JSON con cJSON** (`espressif/cjson`) en los tres firmwares (probado).
- **Solo el gateway depende del WiFi** (decisión de Milton): concentrador y hoja usan `ROUTER_FICTICIO "aura-sin-router"`. Para cambiar de red se edita `credenciales.h` y se regraba solo el gateway (probado: unión y plan B andan con SSID distinto).
- **Descubrimiento del broker:** `BROKER_URI ""` → el gateway pregunta `AURA_BROKER?` por broadcast UDP 18830 y `broker.py` contesta; si pierde el broker 3 veces lo vuelve a buscar (probado). La PC tiene que estar en la misma red, marcada como *Privada* en Windows.
- **Vigilante de envío** (`esp_mesh_send_block_time(5000)` + reconexión forzada tras 3 fallos) en hoja y concentrador: evita que la malla quede trabada (`[WND-RX] timeout`) tras reinicios rápidos; recuperación de ~6 min a ~10–25 s (probado).
- **Secretos fuera del código** (regla del repo público `aura-firmware`): `MESH_ID_BYTES` y `MESH_AP_PASS` están en `main/config_local.h` de cada proyecto (en el `.gitignore`; plantilla `config_local.ejemplo.h`). El gateway tiene además `credenciales.h` (plantilla `credenciales.ejemplo.h`). Publicado en `aura-firmware/Mesh_Red_Completa/`.
- **Guía para el equipo:** `GUIA_NODOS_HOJA.md` explica las reglas para hacer hojas y la tabla de parámetros de red con dónde replicar cada cambio. Mantenerla al día si cambian parámetros o el formato.
- Decisiones: AURA define las políticas; a futuro el concentrador ejecuta las reglas que AURA le configure (guardadas en NVS) y habrá prioridades por proceso (manual local con vencimiento > AURA > automático). Por definir.
- `Broker_MQTT/broker.py`: broker MQTT mínimo + tablero en http://localhost:8080; `led_blink.py` hace parpadear el LED.
- Compilar desde la terminal: `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` y después `idf.py -p COMx build flash`.

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
| `esp_mesh_set_parent` devuelve ESP_FAIL | IDF 6.1 rechaza `MESH_LEAF` en set_parent | Usar `MESH_NODE`; `mesh_ap.max_connection` mínimo 1 |
| El ESP32 no ve el WiFi de la PC | La PC está en 5 GHz | El router también emite en 2,4 GHz; el gateway escanea y fija ese BSSID |
| El LED RGB de la placa S3 no prende | Waveshare ESP32-S3-DEV-KIT: el WS2812 está en GPIO38 (no 48) y usa orden R-G-B | `PIN_LED_RGB 38` y `LED_STRIP_COLOR_COMPONENT_FMT_RGB` |
| El gateway no llega al broker de la PC (timeout) | Regla del firewall de Windows que bloquea python.exe (Privada) | Permitir Python en el firewall en redes privadas (ya hecho en la PC de Milton) |

**Regla general**: corregir el archivo, guardarlo (Auto Save activado), borrar `build` y recompilar.

## Documentación generada
- Guía paso a paso (entorno, creación del proyecto, configuración, build/flash/monitor, tabla de errores y checklist) en Google Docs: https://docs.google.com/document/d/1Do-hS3lmHAnvwI94Fp_e9jW_9CfkiOE6Oma4E44D7vk/edit
  - En la sección 3, los puertos ya están generalizados ("el COM donde aparece la placa en tu PC").
  - **Pendiente opcional**: generalizar también COM7/COM9 en la tabla "Placas usadas en la prueba" y en los títulos "Salida esperada… (COM7/COM9)".

## Próximos pasos posibles
- **Guardar y reenviar (decidido):** cuando un nodo no puede entregar datos (por ejemplo, en el cambio a plan B), los guarda con marca de tiempo y los manda todos juntos al reconectarse. Cada nodo debe tener la hora para ordenar las mediciones; la hora (SNTP en el gateway) y cJSON ya están; falta el buffer en la hoja. Un RTC con pila queda para los nodos que lo necesiten.
- Formato Sparkplug B: queda para después; el equipo prueba primero con el JSON actual.
- Prioridades y reglas locales: Milton avisa cuando estén definidas.
- Raíz con Ethernet + cliente MQTT, publicando en formato Sparkplug B (BIRTH/DATA con alias).
- Concentrador S3 por aula como padre fijo con plan B, y nodos de piso auto-organizados.
- Sensores y actuadores reales en el nodo C3, en lugar de los datos simulados.
- Gateway LoRa → Ethernet para los dispositivos externos.
- OTA manual.
