# Mesh_Red_Completa – prototipo de la red mesh de AURA en ESP-IDF

Prototipo **completo y probado en placas** de la red de un aula, de punta a punta: hoja → concentrador del aula →
gateway (raíz) → MQTT → tablero, y las órdenes en sentido inverso. Está hecho en **ESP-IDF 6.1** (C, sin Arduino).

> **Ojo:** es un banco de pruebas, separado del firmware oficial del repositorio (`infraestructura/`, `dispositivos/` y
> `comun/`, en Arduino). Todavía **no** respeta [`docs/CONTRATO_MQTT.md`](../docs/CONTRATO_MQTT.md): usa un JSON propio,
> descrito en la guía. Sirve para probar la arquitectura (modo aula, plan B, reemplazo de placas sin reprogramar, hora,
> descubrimiento del broker) antes de llevarla al firmware oficial.

```
                        broker MQTT (Broker_MQTT/broker.py) + tablero web
                                          ▲
                                          │ WiFi 2,4 GHz (único nodo que depende del WiFi)
                                ┌─────────┴──────────┐
                                │  Mesh3_Gateway     │  raíz · ESP32-S3
                                └─────────┬──────────┘
                                          │ malla ESP-WIFI-MESH
                                ┌─────────┴──────────┐
                                │  Mesh3_Intermedio  │  concentrador del aula · ESP32-S3
                                │  pantalla + botón  │
                                └─────────┬──────────┘
                                          │
                                ┌─────────┴──────────┐
                                │  Mesh3_Hoja        │  sensores/actuadores · ESP32-C3
                                └────────────────────┘
```

## Qué hay en cada carpeta

| Carpeta / archivo | Qué es |
|---|---|
| [`Mesh3_Gateway/`](Mesh3_Gateway/) | **Gateway (raíz de la malla).** Es el único nodo con WiFi. Publica por MQTT lo que mandan los nodos, en el tópico que elige cada nodo, y reenvía a la malla las órdenes de `aura/cmd/<id>`. Aprende solo por dónde llegar a cada nodo (`id → ruta`), toma la hora de internet (SNTP) y la reparte, y **encuentra el broker solo** por broadcast UDP. Placa: Waveshare ESP32-S3-DEV-KIT, módulo WROOM-2 (flash octal 32 MB). |
| [`Mesh3_Intermedio/`](Mesh3_Intermedio/) | **Concentrador del aula.** Anuncia su aula en el beacon WiFi para que las hojas lo encuentren sin MACs, reenvía todo lo del aula hacia y desde el gateway, muestra los datos ("pantalla": LED RGB + puerto serie) y tiene **mando local** (botón BOOT). Placa: Waveshare ESP32-S3-DEV-KIT, módulo WROOM-1 (flash quad 16 MB). |
| [`Mesh3_Hoja/`](Mesh3_Hoja/) | **Nodo hoja** de ejemplo (datos simulados + LED como actuador). **Modo aula:** se cuelga del concentrador de su aula y le manda cada dato una sola vez. **Plan B:** si el concentrador se cae, se cuelga de la raíz u otro concentrador, y vuelve sola cuando el suyo reaparece. Es la **base para hacer hojas nuevas**. Placa: ESP32-C3 SuperMini. |
| [`Broker_MQTT/`](Broker_MQTT/) | Herramientas de PC, en Python y sin dependencias: `broker.py` (broker MQTT mínimo, tablero en http://localhost:8080 y respuesta al descubrimiento), `dashboard.html` (el tablero), `led_blink.py` (prueba de ida y vuelta del LED) y `LEEME.md` (cómo probar, resultados y notas de diseño). |
| [`GUIA_NODOS_HOJA.md`](GUIA_NODOS_HOJA.md) | **Guía para quienes hagan nodos hoja:** cómo se conectan, reglas del firmware, formato de los mensajes, cómo agregar un actuador, checklist, **tabla de parámetros de red con dónde replicar cada cambio**, alcance y problemas frecuentes. |
| [`CONTEXTO_PROYECTO.md`](CONTEXTO_PROYECTO.md) | Contexto técnico del proyecto: entorno, decisiones de arquitectura, errores resueltos y próximos pasos. Es una copia del `CLAUDE.md` local, con otro nombre porque el repo ignora los `CLAUDE.md`. |

Dentro de cada proyecto ESP-IDF:

| Archivo | Para qué |
|---|---|
| `CMakeLists.txt` | Proyecto (`project(...)`) |
| `sdkconfig.defaults` | Ajustes de la placa (target, flash, particiones). El `sdkconfig` se genera al compilar. |
| `dependencies.lock` | Versiones fijas de los componentes externos (`espressif/cjson`, `espressif/mqtt`, `espressif/led_strip`) |
| `main/main.c` | El firmware, comentado |
| `main/idf_component.yml` | Componentes externos (se descargan solos en el primer build) |
| `main/config_local.ejemplo.h` | Plantilla del **ID y la clave de la mesh** (ver abajo) |
| `main/credenciales.ejemplo.h` | Solo en el gateway: plantilla de la **red WiFi** y del broker |

## Cómo compilar y probar

**Requisitos:** ESP-IDF 6.1 y Python 3 en la PC. Las carpetas tienen que estar en una ruta sin espacios ni tildes, fuera de
Google Drive y OneDrive.

1. **Configuración local (no se sube al repo):**
   - En los tres proyectos, copiar `main/config_local.ejemplo.h` como `main/config_local.h` y completar `MESH_ID_BYTES` y
     `MESH_AP_PASS`. Los da la cátedra y tienen que ser **iguales en los tres**. Sin ese archivo, la compilación avisa con un error.
   - En el gateway, copiar `main/credenciales.ejemplo.h` como `main/credenciales.h` y poner `ROUTER_SSID` y `ROUTER_PASS`.
     La red tiene que ser de **2,4 GHz**, sin portal de login y sin aislamiento de clientes. Si `BROKER_URI` queda vacío,
     el gateway encuentra el broker solo.
2. **Compilar y grabar** cada proyecto, desde su carpeta:
   ```bash
   idf.py set-target esp32s3        # esp32c3 para Mesh3_Hoja
   idf.py -p COMx build flash monitor
   ```
3. **Broker y tablero** en la PC, que tiene que estar en la misma red que el gateway y, en Windows, con perfil de red *Privada*:
   ```bash
   python Broker_MQTT/broker.py
   ```
   Abrir http://localhost:8080. En alrededor de un minuto aparecen los datos de la hoja y del concentrador. Los botones del
   tablero (o `python Broker_MQTT/led_blink.py`) comandan el LED de la hoja, y el botón BOOT del concentrador también.

Para cambiar de red WiFi se edita **solo** `credenciales.h` y se regraba **solo el gateway**: ningún otro nodo depende del WiFi.

## Tópicos MQTT

| Sentido | Tópico | Ejemplo de payload |
|---|---|---|
| Hoja → MQTT | `aura/aula1/sensores` | `{"id":"aula1-hoja","mac":"…","modo":"aula","ts":1792423469362,"temp":24.1,"hum":55,"pres":true,"led":0,"rssi":-45,"via":"…"}` |
| Hoja → MQTT | `aura/aula1/led` | `{"id":"aula1-hoja","led":1,"origen":"aura","ts":…,"via":"…"}` (`origen`: `aura` o `local`) |
| Concentrador → MQTT | `aura/aula1/concentrador/estado` | `{"id":"aula1-concentrador","capa":2,"hijos":1,"nodos":3,"led_hoja":1,"ts":…}` |
| MQTT → nodo | `aura/cmd/<id>` | `{"led":1}`, `{"led":0}`, `{"led":"toggle"}` |

`ts` = hora de la medición en ms UTC. El detalle de los campos está en la guía, sección 2.

## Qué se probó en placas

- Malla de 3 niveles con MQTT de subida y de bajada (ida y vuelta del LED: unos 20 ms en una red doméstica y 80–240 ms en
  una red con más tráfico).
- **Reemplazo de placas sin reprogramar nada**: ningún firmware tiene MACs. Se simuló con placas de otra MAC.
- **Plan B**: con el concentrador apagado, la hoja pasa a la raíz en unos 30 s, AURA la sigue comandando, y la hoja vuelve
  sola a su concentrador.
- **Hora**: SNTP en el gateway, repartida a los nodos (diferencia con la PC menor a 1 s).
- **Cambio de red WiFi** regrabando solo el gateway; el broker se encontró solo en la IP nueva.
- **Recuperación tras reinicios rápidos** de la raíz o de un padre: en unos 10–25 s, gracias a un vigilante de envío.
  Sin él, la malla quedaba trabada unos 6 minutos.
- **Alcance** de la C3 SuperMini: unos 10 m a través de una vidriera, con antena de alambre de 31 mm y 15 dBm.
  Ver la guía, sección 6.

Los resultados con fecha y los detalles están en [`Broker_MQTT/LEEME.md`](Broker_MQTT/LEEME.md).
