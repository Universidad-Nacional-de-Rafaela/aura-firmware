# Prueba AURA: malla de 3 nodos + MQTT

```
Hoja C3 (COM9) ──malla──> Concentrador S3 (COM11) ──malla──> Gateway S3 (COM12) ──WiFi──> Router ──> broker.py (PC)
   capa 3                    capa 2 (pantalla + botón)        capa 1 (raíz)
```

| Proyecto | Carpeta | id lógico | Placa | Puerto flash/monitor | MAC (solo informativa) |
|---|---|---|---|---|---|
| Gateway | `Mesh3_Gateway` | (raíz) | Waveshare ESP32-S3-DEV-KIT-NxRx, módulo ESP32-S3-WROOM-2 N32R16V (flash octal 32 MB) | COM12 (CH343) | 90:e5:b1:d6:4a:94 |
| Concentrador | `Mesh3_Intermedio` | `aula1-concentrador` | Waveshare ESP32-S3-DEV-KIT-NxRx, módulo ESP32-S3-WROOM-1 N16R8 (flash quad 16 MB) | COM11 (CH343) | dc:b4:d9:2a:58:78 |
| Hoja | `Mesh3_Hoja` | `aula1-hoja` | ESP32-C3 SuperMini (4 MB) | COM9 | 90:70:69:ad:d9:64 |

COM13 y COM10 son el USB nativo de cada S3: sirven para flashear, pero la consola sale por el CH343.

**Placas Waveshare ESP32-S3-DEV-KIT-NxRx** (gateway y concentrador tienen la misma placa con distinto módulo):
el LED RGB es un WS2812 en **GPIO38** con orden de color **R-G-B** (no el G-R-B habitual), no en el GPIO48 de las DevKitC de Espressif.
Si se configura con G-R-B, el verde sale rojo. El botón BOOT es GPIO0. Pinout: [circuitstate.com](https://www.circuitstate.com/pinouts/waveshare-esp32-s3-dev-kit-nxr8-wi-fi-development-board-pinout-diagram-arduino-reference/).

## Reemplazo de un nodo

**Ningún firmware ni AURA usa MACs.** Para reemplazar una placa: se graba el mismo firmware y listo.
- Cada nodo tiene en su firmware su `AULA` y su `DEVICE_ID`, y manda `"id"` en todos sus JSON.
  El gateway arma solo la tabla `id → ruta` con el último mensaje de cada nodo y loguea
  `Nodo aula1-hoja cambió de ruta (otro padre o placa reemplazada): <MAC vieja> -> <MAC nueva>`.
- El concentrador anuncia en su beacon un *vendor IE* de AURA (OUI `02:A5:52`, tipo 1) con su aula y su MAC de malla.
  La hoja se cuelga del concentrador que anuncia **su** aula.
- El concentrador aprende la MAC de la hoja de sus mensajes.
- Probado simulando placas nuevas (otra MAC con `esp_base_mac_addr_set`): la hoja encontró al "nuevo" concentrador,
  el gateway detectó el cambio y `led_blink.py` siguió funcionando sin tocar nada.

Para otra aula se cambian solo `AULA` (y con eso `DEVICE_ID` y los tópicos) en la hoja y en el concentrador.

## Tópicos

| Sentido | Tópico | Payload |
|---|---|---|
| Hoja → MQTT | `aura/aula1/sensores` | `{"via":"…","id":"aula1-hoja","mac":"…","modo":"aula","n":12,"capa":3,"temp":24.1,"hum":55.0,"pres":true,"led":0}` |
| Hoja → MQTT | `aura/aula1/led` | `{"via":"…","id":"aula1-hoja","mac":"…","modo":"aula","led":1,"origen":"aura","n":5}` (`origen`: `aura` o `local`) |
| Concentrador → MQTT | `aura/aula1/concentrador/estado` | `{"via":"…","id":"aula1-concentrador","mac":"…","n":3,"capa":2,"hijos":1,"hojas_aula":1,"nodos":3,"led_hoja":1}` |
| MQTT → nodo | `aura/cmd/aula1-hoja` | `{"led":1}`, `{"led":0}` o `{"led":"toggle"}` |

- `mac` = MAC propia del nodo (informativa). `via` = MAC del nodo de la malla que le entregó el mensaje al gateway
  (el concentrador en modo aula, la propia hoja en plan B). `modo` = `aula` o `directo`.
- El nodo pone el tópico en el campo `"topic"` de su JSON. El gateway lo saca, agrega `"via"` y publica.
- Para bajar una orden a cualquier nodo: `aura/cmd/<id>`. El gateway la manda por la última ruta conocida de ese id
  y le agrega `"dest":"<id>"` adelante (`{"dest":"aula1-hoja","led":1}`). El resto del JSON pasa sin cambios,
  así que campos futuros como `"prio"` no requieren tocar la red. Si el id todavía no mandó ningún mensaje, el gateway lo informa y la descarta.

## Modo aula y plan B de la hoja

```
MODO AULA (normal)    Hoja ──1 mensaje──> Concentrador ──> pantalla + reenvío ──> Raíz ──> MQTT
                      MQTT ──> Raíz ──{"dest":"aula1-hoja"}──> Concentrador ──> Hoja

PLAN B (sin concentr.) Hoja ──────────────> Raíz (u otro concentrador, nunca otra hoja) ──> MQTT
```

- **Modo aula:** la hoja se cuelga de su concentrador y le manda cada mensaje **una sola vez**. El concentrador lo muestra y lo
  reenvía a la raíz. Las órdenes de AURA le llegan con `"dest"` y el concentrador se las pasa a la hoja que corresponde.
  Así no se duplica tráfico en el canal WiFi del aula.
- **Plan B:** si pierde al concentrador, le da 5 s para volver (cortes breves) y después lo busca. Si no lo ve en 3 escaneos
  seguidos, se cuelga de la **raíz o de otro concentrador** (el de menor capa y mejor señal) y manda **directo a la raíz**.
- **Vuelta:** en plan B, cada 30 s escanea solo el canal actual (rápido, sin cortar la conexión). Si su concentrador volvió, se pasa a él.
- **AURA no se entera de nada de esto:** el gateway sigue la ruta del último mensaje de cada `id`, y los tópicos son los mismos.
- Tiempos (`Mesh3_Hoja/main/main.c`): `ESPERA_RECONEXION_MS` 5 s, `BUSQUEDAS_PLAN_B` 3, `REVISAR_AULA_MS` 30 s.
- **Detalle de ESP-WIFI-MESH:** con la auto-organización apagada, la pila reintenta sin fin el último padre y mientras tanto
  el WiFi rechaza los escaneos (`STA is connecting, scan are not allowed!`). La hoja corta esos reintentos con
  `esp_mesh_set_self_organized(false, false)` antes de buscar otro padre.

**Prueba (concentrador mantenido en reset por RTS durante 110 s):** la hoja lo perdió a los 58 s y pasó a plan B colgada de la raíz
a los 91 s (unos 33 s sin datos, que no se guardan). AURA siguió comandando el LED en plan B (23–24 ms). Cuando el concentrador
volvió, la hoja regresó a modo aula en el chequeo siguiente. El gateway cambió la ruta solo en los dos sentidos.

## Pantalla y mando local del concentrador

- El concentrador recibe los mensajes de las hojas de su aula porque **van dirigidos a él** (modo aula).
  No puede leer lo que solo pasa a través suyo: la pila mesh lo reenvía sin entregárselo a la aplicación.
- "Pantalla" (por ahora): el LED RGB WS2812 de la placa (Waveshare: **GPIO38**, orden de color **R-G-B**; brillo en `BRILLO_LED`) se prende en azul según el estado que **informa** la hoja,
  y el puerto serie muestra líneas `PANTALLA aula1 | …` con los sensores y el LED. El estado del LED también viaja en el
  mensaje de sensores cada 2 s, así la pantalla y el tablero se resincronizan solos si se pierde una respuesta.
- Mando local: el botón **BOOT (GPIO0)** manda `{"led":"toggle","origen":"local"}` directo a la hoja, sin gateway ni MQTT.
  La hoja responde como siempre, así que el tablero también se entera (cuenta los "toques del botón local").

## Hora y marca de tiempo

- El gateway toma la hora de internet (SNTP, `pool.ntp.org`) apenas tiene IP y la reparte cada 60 s a todos los nodos
  conocidos con `{"dest":"<id>","hora":<ms UTC>}`. También la manda apenas aparece un nodo o cambia de ruta.
- Cada nodo pone `"ts"` (ms desde 1970, UTC) en sus mensajes. Si un mensaje llega sin `ts` (el nodo todavía no tiene hora),
  el gateway le pone la suya y agrega `"ts_gw":true`.
- El ESP32 no tiene pin de batería para su RTC: sin alimentación pierde la hora. Para conservarla tras un corte de energía
  hace falta un RTC externo con pila (DS3231 o PCF8563). Queda para más adelante.
- Probado: el gateway sincronizó en 5 s, la hoja recibió la hora unos 15 s después de conectarse, y el `ts` de cada
  medición coincide con el reloj de la PC con menos de 1 s de diferencia.

Los JSON se leen y se arman con **cJSON** (`espressif/cjson`) en los tres firmwares. Ver [GUIA_NODOS_HOJA.md](../GUIA_NODOS_HOJA.md).

## Cómo probar

1. **Firewall (una sola vez, como administrador):** Windows tiene una regla que *bloquea* `python.exe` en redes privadas,
   así que el gateway no llega al broker. En "Firewall de Windows Defender → Permitir una aplicación" habilitá Python en *Privada*,
   o borrá las reglas "python.exe (Bloquear)" y aceptá el aviso la próxima vez que arranque el broker.
2. Broker + tablero: `python broker.py` en esta carpeta → abrir **http://localhost:8080** en Chrome.
3. Encender las 3 placas (el orden no importa; la hoja espera a que el concentrador esté en la malla).
4. LED: botones del tablero, o `python led_blink.py` (lo hace parpadear cada 1 s y mide ida y vuelta).

## Desconectar y volver a armar todo

Los firmwares quedan grabados en el flash: **no hay que volver a flashear nada**. Solo:

1. **Alimentar las placas.** Cualquier USB sirve, incluso un cargador. No importa el orden ni el puerto. Los COM solo importan para flashear o mirar el monitor, y pueden cambiar si se usa otro puerto USB.
2. **En la PC, levantar el broker** (`python broker.py`) y abrir http://localhost:8080. El gateway encuentra el broker solo
   (no hace falta saber la IP de la PC) y se conecta apenas el broker está arriba.
3. Esperar alrededor de 1 minuto. Unos 10 s después de encender, el gateway tiene router, MQTT y hora. Luego entra el concentrador. La hoja
   se cuelga del concentrador; si arranca mucho antes que él, pasa un rato por plan B y vuelve sola en el siguiente chequeo (30 s).

### En otra red WiFi

1. Editar **solo** `Mesh3_Gateway/main/credenciales.h` (`ROUTER_SSID` y `ROUTER_PASS`) y **regrabar solo el gateway**.
   Concentrador y hoja no dependen del WiFi y no se tocan. La IP de la PC no hace falta: el gateway la descubre.
2. La red tiene que tener **2,4 GHz**, **no** tener portal de login ni aislamiento de clientes (muchas redes institucionales
   lo tienen: en ese caso, usar un router propio o el hotspot del celular).
3. En la PC, Windows tiene que marcar la red como **Privada** (Configuración → Red e Internet → la red → Privada).
   Si queda como *Pública*, el firewall bloquea el broker y el gateway muestra `No encuentro el broker`.
4. La PC y el gateway tienen que estar en **la misma red** (el descubrimiento es por broadcast y no cruza routers).

**Probado en la red de la facultad (IngComputacion, 2,4 GHz, canal 1):** solo se cambió `credenciales.h` y se regrabó el gateway.
El gateway encontró el broker solo (la PC tenía otra IP: 192.168.0.115), sincronizó la hora, y el concentrador y la hoja se
reacomodaron solos al canal nuevo sin regrabarlos. La ida y vuelta del LED subió a 77–242 ms por el tráfico de la red.

**Alcance de la hoja (C3 SuperMini):** a 10 dBm llegaba a ~3 m con una pared. Con una antena de alambre de 31 mm soldada y
15 dBm (`POTENCIA_TX 60`) llega a ~10 m a través de una vidriera, alimentada con powerbank. Con la antena puesta y 19,5 dBm
andaba solo conectada a la PC, no con cargador ni powerbank. El tablero muestra la "señal del padre" (`rssi`), y el LED azul da
señales de vida: 2 destellos al arrancar y 1 largo al conectarse.

## Archivos que se tocan seguido

- `Mesh3_Gateway/main/credenciales.h`: SSID y clave del router. `BROKER_URI` vacío = descubrir el broker solo.
- `Mesh3_Hoja/main/main.c` y `Mesh3_Intermedio/main/main.c`: bloque "Identidad de este equipo" (`AULA`, `DEVICE_ID`, tópicos).
- `Mesh3_Intermedio/main/main.c`: `PIN_LED_RGB` (38 en las Waveshare; 48 en otras S3), `BRILLO_LED` y `PIN_BOTON`.

## Notas de diseño

- La raíz busca el router antes de armar la malla y usa su canal de **2,4 GHz**. La PC está en 5 GHz con el mismo SSID; el ESP32 no ve esa banda.
- El intermedio y la hoja arrancan con canal 0, así que buscan en todos los canales y siguen a la raíz.
- La hoja tiene desactivada la auto-organización y elige padre por aula (vendor IE). Si en 15 s no se conecta, vuelve a escanear.
- **Solo el gateway depende del WiFi.** Concentrador y hoja usan un SSID ficticio fijo (`ROUTER_FICTICIO`), distinto del real:
  probado que se unen y que el plan B (hoja colgada directo del gateway) funciona igual.
- **Descubrimiento del broker:** con `BROKER_URI` vacío, el gateway pregunta `AURA_BROKER?` por broadcast UDP 18830 apenas tiene IP,
  y broker.py contesta. Probado: lo encuentra en menos de 1 s, y si el broker se apaga, después de 3 desconexiones lo vuelve a
  buscar hasta que aparece.
- **Vigilante de envío:** si un nodo se reinicia muy rápido, su hijo puede quedar con el control de flujo de la malla trabado
  (`[WND-RX] … timeout`) y la pila tarda unos 6 min en recuperarse sola. Con `esp_mesh_send_block_time(5000)` y una reconexión
  forzada tras 3 envíos fallidos, se recupera en unos 10–25 s. Probado reiniciando el gateway dos veces seguidas (recuperación en 11 s)
  y el concentrador por 1 s (la hoja siguió en modo aula y perdió 3 mensajes).
- **Detalle de IDF 6.1:** `esp_mesh_set_parent(..., MESH_LEAF, ...)` devuelve `ESP_FAIL`, con o sin `esp_mesh_set_type(MESH_LEAF)`.
  Por eso la hoja se une como `MESH_NODE`. Además `mesh_ap.max_connection = 0` no se admite, así que queda en 1:
  en teoría un nodo podría colgarse de la hoja, pero ninguno lo hace porque los demás eligen al padre de menor capa.

## Resultado de la prueba (19/10/2026, noche)

- ✅ Gateway: encuentra Personal-419 en 2,4 GHz (canal 11), se conecta y obtiene la IP 192.168.1.35.
- ✅ Intermedio: se une al gateway en la capa 2, canal 11.
- ✅ Hoja: se une **solo** al intermedio en la capa 3, y el gateway recibe sus JSON a través del intermedio (3 nodos en la tabla de ruteo).
- ✅ Broker, tablero y `led_blink.py`: probados en la PC con un gateway simulado (ida, vuelta y botones).
- ❌ Gateway → broker: el gateway no llegaba a 192.168.1.5:1883 (timeout) por la regla del firewall que bloqueaba python.exe.

## Resultado con MQTT real (20/10/2026)

- ✅ Firewall habilitado: el gateway (192.168.1.35) se conecta al broker y publica en el tópico que eligió cada nodo,
  sin el campo `topic` y con `mac` agregado.
- ✅ La malla funcionó toda la noche sin reinicios (la hoja llegó a n = 14 181, unas 8 h, sin mensajes perdidos).
- ✅ LED ida y vuelta (PC → gateway → intermedio → hoja → PC): con `led_blink.py`, 20 comandos y 20 respuestas,
  con un tiempo de ida y vuelta de 14 a 27 ms (promedio 19 ms).
- ✅ El tablero muestra los sensores, el estado del intermedio y el LED en vivo; los botones Prender, Apagar, Parpadear y Detener funcionan con la placa real.
- En ESP-IDF 6.x MQTT es un componente externo (`espressif/mqtt`, en `main/idf_component.yml`). Se baja solo en el primer build y queda en `managed_components/`.
