# AURA – Guía para desarrollar nodos hoja

Esta guía es para quienes programen **nodos hoja** (sensores y actuadores de un aula) que se conectan a la malla
ESP-WIFI-MESH de AURA. Explica cómo se conecta una hoja a su concentrador, qué tiene que cumplir su firmware y qué
parámetros de red se pueden cambiar, indicando **en qué firmwares hay que replicar cada cambio**.

El ejemplo de referencia es `Mesh3_Hoja/main/main.c` (ESP32-C3 SuperMini). Para una hoja nueva conviene **copiar
ese proyecto** y modificar solo lo que se indica acá.

---

## 1. Cómo funciona la red

```
                           MQTT (broker / AURA)
                                   ▲
                                   │ WiFi del router (2,4 GHz)
                         ┌─────────┴─────────┐
                         │  GATEWAY (raíz)   │  capa 1 · ESP32-S3
                         └─────────┬─────────┘
                    malla          │
            ┌──────────────────────┴───────────────────┐
   ┌────────┴─────────┐                       ┌─────────┴────────┐
   │ CONCENTRADOR     │  capa 2 · ESP32-S3    │ CONCENTRADOR     │
   │ aula1            │  pantalla + botones   │ aula2            │
   └───┬─────────┬────┘                       └───┬──────────────┘
       │         │                                │
   ┌───┴───┐ ┌───┴───┐                        ┌───┴───┐
   │ hoja  │ │ hoja  │  capa 3 · ESP32-C3     │ hoja  │
   └───────┘ └───────┘                        └───────┘
```

| Rol | Qué hace |
|---|---|
| **Gateway** | Raíz única de la malla. Puente entre la malla y MQTT. Toma la hora de internet y la reparte. |
| **Concentrador** | Uno por aula. Es el padre de las hojas de su aula, muestra sus datos (pantalla), tiene mando local y reenvía todo hacia y desde el gateway. |
| **Hoja** | Sensores y actuadores. Se cuelga del concentrador de **su** aula. |

### 1.1 Cómo encuentra la hoja a su concentrador (sin MACs)

**Ningún firmware tiene MACs escritas.** Cada concentrador anuncia en su beacon WiFi un *vendor IE* de AURA que dice
"soy el concentrador de `aula1`" e incluye su dirección en la malla. La hoja escanea y elige como padre al nodo
que anuncia **su misma aula** (`AULA`).

Por eso **si se rompe una placa, se graba el mismo firmware en otra y listo**: las hojas encuentran al
concentrador nuevo y el gateway aprende la ruta nueva con el primer mensaje.

### 1.2 Modo aula y plan B

| Modo | Cuándo | Padre | A quién manda | Campo `modo` |
|---|---|---|---|---|
| **Aula** (normal) | Ve a su concentrador | Su concentrador | **Solo** al concentrador, que muestra y reenvía | `"aula"` |
| **Plan B** | No ve a su concentrador | La raíz u **otro concentrador** (nunca otra hoja) | Directo a la raíz | `"directo"` |

- En modo aula cada mensaje sale **una sola vez**: no se duplica tráfico en el canal WiFi del aula.
- Si la hoja pierde al concentrador, le da `ESPERA_RECONEXION_MS` (5 s) para volver. Si no vuelve, lo busca,
  y después de `BUSQUEDAS_PLAN_B` (3) escaneos sin verlo pasa a plan B (unos 30 s en total).
- En plan B revisa cada `REVISAR_AULA_MS` (30 s) si su concentrador volvió, con un escaneo rápido del canal actual,
  y si volvió se pasa a él.
- **AURA no nota nada:** los tópicos son los mismos y el gateway sigue la ruta del último mensaje de cada `id`.

> Toda esta lógica ya está resuelta en el ejemplo (`procesar_escaneo`, `tarea_vigia`, `conectar`). **No hace falta
> tocarla** para hacer una hoja nueva.

---

## 2. Qué tiene que cumplir el firmware de una hoja

### 2.1 Reglas obligatorias

1. **Parámetros comunes de malla idénticos** a los del resto de la red (`MESH_ID_BYTES` y `MESH_AP_PASS` en
   `main/config_local.h`, y `MESH_MAX_CAPAS`; ver la sección 4). `config_local.h` no se sube al repositorio. Si uno solo difiere, la hoja no se une.
2. **`AULA` igual a la del concentrador** del aula donde se instala. Es lo que usa para elegir padre.
3. **`DEVICE_ID` único en todo el edificio** (ver 2.2). Es el nombre con el que AURA la conoce.
4. **Mandar siempre con `publicar()` / `publicar_json()`.** No llamar a `esp_mesh_send()` directo: esas funciones
   eligen el destino según el modo (concentrador o raíz).
5. **Todo mensaje lleva los campos comunes**: usar `mensaje_nuevo(topico)`, que agrega `topic`, `id`, `mac`,
   `modo` y `ts`.
6. **Responder cada orden con el estado real** del actuador (lo medido, no lo pedido) e incluir ese estado también
   en el mensaje periódico. Así la pantalla y AURA se resincronizan aunque se pierda una respuesta.
7. **No usar `esp_mesh_set_type(MESH_LEAF)`** ni `MESH_LEAF` en `esp_mesh_set_parent()`: en IDF 6.1 fallan
   (`ESP_FAIL`). La hoja se une como `MESH_NODE` con `mesh_ap.max_connection = 1` (el mínimo que acepta la API).
8. **Auto-organización apagada** (`esp_mesh_set_self_organized(false, false)` en `MESH_EVENT_STARTED`). El padre
   lo elige el firmware, no la pila.
9. **No bloquear** el manejador de eventos de la malla ni el callback del vendor IE (`vendor_ie_cb`): corren en
   tareas del sistema. Nada de `vTaskDelay` ni esperas largas ahí.
10. **JSON con cJSON** (componente `espressif/cjson` en `main/idf_component.yml`). Liberar siempre lo que se crea:
    `cJSON_Delete()` para los objetos y `cJSON_free()` para los textos de `cJSON_PrintUnformatted()`.
    `publicar_json()` ya libera el objeto que recibe.
11. **Mensajes de menos de 500 bytes.** El concentrador recibe con un buffer de 600 bytes y la hoja recibe órdenes
    con otro de 600. Si hace falta más, agrandar esos buffers (ver la sección 4).
12. **Mantener el vigilante de envío** (`esp_mesh_send_block_time` y el conteo de `fallos_envio` en `publicar()`).
    Si el padre se reinicia muy rápido, la hoja puede quedar "conectada" con el control de flujo de la malla trabado
    (`[WND-RX] … timeout` en el monitor) y sin poder enviar nada. Sin el vigilante, la pila tarda unos 6 minutos en
    recuperarse. Con el vigilante, después de 3 envíos fallidos la hoja fuerza la reconexión: se recupera en unos 20 s.
13. **No depender del WiFi del edificio.** La hoja no lleva el SSID ni la clave del router (usa `ROUTER_FICTICIO`).

### 2.2 Nombres

| Elemento | Formato | Ejemplo |
|---|---|---|
| `AULA` | minúsculas, sin espacios ni tildes | `aula1`, `lab-redes` |
| `DEVICE_ID` | `<AULA>-<tipo>[-<n>]`, **único**, menos de 32 caracteres | `aula1-hoja`, `aula1-ventana-2` |
| Tópico de datos | `aura/<AULA>/<qué>` | `aura/aula1/sensores`, `aura/aula1/ventana` |

> El concentrador reconoce como "hojas de su aula" a los `id` que empiezan con `<AULA>-`. Respetar ese prefijo.

### 2.3 Mensajes que manda la hoja

Todos son objetos JSON. Los campos comunes los arma `mensaje_nuevo()`:

| Campo | Tipo | Lo pone | Descripción |
|---|---|---|---|
| `topic` | texto | hoja | Tópico MQTT donde el gateway lo publica. El gateway lo saca del payload. |
| `id` | texto | hoja | `DEVICE_ID`. Con esto AURA y el gateway identifican el nodo. |
| `mac` | texto | hoja | MAC propia, solo informativa (no se usa para rutear). |
| `modo` | texto | hoja | `aula` o `directo` (plan B). |
| `ts` | número | hoja | Hora de la medición en **milisegundos desde 1970, UTC**. No aparece hasta que la hoja recibe la hora. |
| `ts_gw` | `true` | gateway | Aparece si el mensaje llegó sin `ts` y el gateway puso su propia hora. |
| `via` | texto | gateway | MAC del nodo que le entregó el mensaje: el concentrador en modo aula, la hoja en plan B. |
| `n` | número | hoja | Contador del mensaje (sirve para detectar pérdidas). |

Ejemplo de mensaje periódico, tal como sale de la hoja y tal como lo publica el gateway:

```jsonc
// la hoja manda
{"topic":"aura/aula1/sensores","id":"aula1-hoja","mac":"90:70:69:ad:d9:64","modo":"aula",
 "ts":1792423469362,"n":27,"capa":3,"temp":27,"hum":46.5,"pres":true,"led":0}

// MQTT, tópico aura/aula1/sensores
{"id":"aula1-hoja","mac":"90:70:69:ad:d9:64","modo":"aula","ts":1792423469362,"n":27,
 "capa":3,"temp":27,"hum":46.5,"pres":true,"led":0,"via":"dc:b4:d9:2a:58:78"}
```

> **Números con decimales:** generarlos como `double` y redondeados (ver `tarea_envio`). Si se pasa un `float`,
> cJSON escribe `24.100000381` en lugar de `24.1`.

### 2.4 Mensajes que recibe la hoja

| Mensaje | Viene de | Qué hacer |
|---|---|---|
| `{"hora":1792423428264,"dest":"aula1-hoja"}` | Gateway, cada 60 s y apenas la hoja aparece | Ya resuelto: `hora_ajustar()` pone el reloj. |
| `{"led":1,"dest":"aula1-hoja"}` | AURA (MQTT `aura/cmd/aula1-hoja`) | Ejecutar y responder con el estado real. |
| `{"led":"toggle","origen":"local"}` | Botón del concentrador | Igual, y responder con `"origen":"local"`. |

- **`dest`** lo agrega el gateway para que el concentrador sepa a qué hoja pasar la orden. La hoja puede ignorarlo.
- **`origen`**: si no viene, la orden es de AURA. En la respuesta se pone `"origen":"aura"` o `"origen":"local"`.
- **Campos desconocidos se ignoran.** Así se pueden agregar cosas (por ejemplo, un futuro `"prio"`) sin romper
  hojas viejas.

### 2.5 Cómo agregar un actuador nuevo (ejemplo: ventana)

1. Definir el tópico: `#define TOPICO_VENTANA "aura/" AULA "/ventana"`.
2. En `tarea_ordenes`, leer el campo nuevo con cJSON:
   ```c
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(orden, "ventana");   /* % de apertura */
   if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 100) {
       ventana_mover(v->valueint);                       /* con fines de carrera y tiempo máximo */
       cJSON *resp = mensaje_nuevo(TOPICO_VENTANA);
       cJSON_AddNumberToObject(resp, "ventana", ventana_posicion_medida());  /* la REAL */
       cJSON_AddStringToObject(resp, "origen", local ? "local" : "aura");
       publicar_json(resp);
   }
   ```
3. Agregar la posición medida al mensaje periódico (regla 6).
4. **Seguridades en la propia hoja**: fines de carrera, tiempo máximo de motor y posición segura si pierde la
   comunicación por mucho tiempo. No dependen de AURA ni del concentrador.
5. Para mostrarlo en la pantalla del concentrador, agregarlo en `mostrar_en_pantalla()` de `Mesh3_Intermedio`.

---

## 3. Checklist para una hoja nueva

- [ ] Copié `Mesh3_Hoja` a una carpeta nueva (sin espacios ni tildes, fuera de Google Drive/OneDrive).
- [ ] Cambié el nombre en `project(...)` del `CMakeLists.txt` raíz.
- [ ] Copié `main/config_local.ejemplo.h` como `main/config_local.h` y completé el ID y la clave de la mesh (los da la cátedra; ese archivo **no se sube** al repositorio).
- [ ] Ajusté `sdkconfig.defaults` a la placa (target, tamaño de flash) y borré `sdkconfig` y `build`.
- [ ] Puse `AULA` igual a la del concentrador y un `DEVICE_ID` único.
- [ ] Revisé que los parámetros comunes de malla (sección 4.1) coinciden con el resto de la red.
- [ ] Ajusté pines (`PIN_LED`, `LED_ACTIVO_BAJO`, sensores, actuadores).
- [ ] Ajusté `POTENCIA_TX` a la placa (C3 SuperMini con antena de alambre: 60 = 15 dBm probado). Probé el alcance real mirando la "señal del padre" en el tablero.
- [ ] Todo mensaje sale con `mensaje_nuevo()` + `publicar_json()`.
- [ ] Cada orden responde con el estado medido, y ese estado también va en el mensaje periódico.
- [ ] Probé: en el monitor aparece `Conectada a … MODO AULA` y en MQTT llegan los datos con `ts`.
- [ ] Probé el plan B apagando el concentrador: la hoja pasa a `directo` y vuelve sola a `aula`.

---

## 4. Parámetros de red y dónde replicarlos

**G** = `Mesh3_Gateway/main/main.c` · **Gc** = `Mesh3_Gateway/main/credenciales.h` ·
**C** = `Mesh3_Intermedio/main/main.c` (concentrador) · **H** = `Mesh3_Hoja/main/main.c` (y toda hoja nueva) ·
**PC** = `Broker_MQTT/` (broker, tablero y scripts de prueba)

Después de cambiar un parámetro hay que **recompilar y grabar todos los firmwares donde aparece**.

### 4.1 Comunes a toda la malla: deben ser IDÉNTICOS

| Parámetro | Valor actual | Dónde | Efecto / cuidado |
|---|---|---|---|
| `MESH_ID_BYTES` | (en `config_local.h`) | G, C, H — archivo `main/config_local.h` de cada proyecto | Identifica la red. Nodos con otro ID no se ven entre sí. Cambiarlo permite separar redes (por ejemplo, una de pruebas). |
| `MESH_AP_PASS` | (en `config_local.h`) | G, C, H — archivo `main/config_local.h` de cada proyecto | Clave WPA2 entre nodos (mínimo 8 caracteres). La da la cátedra. |
| `MESH_MAX_CAPAS` | `6` | G, C, H | Profundidad máxima del árbol. La hoja también la usa para descartar padres con capa inválida. |
| `ROUTER_FICTICIO` | `aura-sin-router` | C, H | La API de la malla exige un SSID de router, pero estos nodos **nunca** se conectan al router. Es un valor fijo: **no se cambia al mudar la red**. Probado: se unen y el plan B funciona aunque difiera del SSID real del gateway. |
| Vendor IE de AURA (`AURA_OUI_0..2`, `AURA_IE_CONCENTRADOR`) | `02:A5:52`, tipo `1` | C, H | Es como la hoja reconoce a los concentradores. Si no coincide, la hoja nunca encuentra padre. |

### 4.2 Solo en el gateway

> **El gateway es el único nodo que depende del WiFi.** Para cambiar o agregar una red se edita `credenciales.h` y se
> regraba **solo el gateway**. Concentradores y hojas no se tocan.

| Parámetro | Valor actual | Dónde | Efecto / cuidado |
|---|---|---|---|
| `REDES_WIFI` | `Personal-419`, `IngComputacion` (claves en el archivo) | Gc | Lista de redes `{"SSID", "clave"}` en orden de preferencia (2,4 GHz, sin portal de login y sin aislamiento de clientes). Usa la primera que esté al alcance; si queda 5 min sin router (`REINICIO_SIN_ROUTER_MS`, en G), se reinicia y empieza por la siguiente. |
| `BROKER_URI` | `""` (vacío) | Gc | Vacío = el gateway prueba primero el **último broker que le anduvo en esa red** (guardado en NVS) y, si no responde 3 veces, **lo busca solo** por broadcast UDP y no hace falta saber la IP de la PC. `"mqtt://IP:puerto"` = dirección fija (para un broker que no responda el descubrimiento, como Mosquitto o el servidor de AURA). |
| `PUERTO_DESCUBRIMIENTO` | `18830` (UDP) | G **y** PC (`broker.py`) | Puerto de la pregunta `AURA_BROKER?`. Tiene que coincidir en los dos. |
| `FALLOS_PARA_REBUSCAR` | `3` | G | Desconexiones seguidas del broker antes de volver a buscarlo (por si la PC cambió de IP). |
| Canal de la malla | el del router en 2,4 GHz | automático | El gateway busca el router y usa su canal. Concentradores y hojas usan canal `0` (buscan en todos). **No fijar el canal** en C ni H. |
| `CANAL_SIN_ROUTER` | `6` | G | Canal que usa el gateway si no encuentra el router (la malla anda, pero sin MQTT). |
| `MESH_MAX_HIJOS` | `6` | G | Hijos directos del gateway (máximo 10). |
| `TOPICO_CMD_PREFIJO` | `aura/cmd/` | G **y** PC | Prefijo de las órdenes. Si cambia, cambiarlo también en `broker.py`, `dashboard.html` y `led_blink.py` (y en AURA). |
| `TOPICO_POR_DEFECTO` | `aura/sin_topico` | G | Dónde publica los mensajes que llegan sin `topic` o con JSON inválido. |
| `SERVIDOR_NTP` | `pool.ntp.org` | G | Servidor de hora. En la red de la facultad puede convenir el NTP institucional. |
| `ZONA_HORARIA` | `<-03>3` (UTC-3) | G | Solo afecta los logs. `ts` siempre viaja en UTC. |
| `PERIODO_HORA_MS` | `60000` | G | Cada cuánto reparte la hora. Más corto = relojes más parejos, con un poco más de tráfico. |
| `MAX_NODOS` | `32` | G | Tamaño de la tabla `id → ruta`. Con más nodos en la red hay que agrandarla. |

### 4.3 Solo en el concentrador

| Parámetro | Valor actual | Dónde | Efecto / cuidado |
|---|---|---|---|
| `AULA` | `aula1` | C **y** las H de esa aula | Tiene que ser **la misma** en el concentrador y en sus hojas. |
| `DEVICE_ID` / `TOPICO_ESTADO` | `aula1-concentrador`, `aura/aula1/concentrador/estado` | C (y PC si cambian) | El tablero de prueba busca esos tópicos. |
| `MESH_MAX_HIJOS` | `6` | C | Máximo de hojas directas del aula (máximo 10 por AP). |
| `MAX_HOJAS` | `8` | C | Tamaño de la tabla de hojas del aula. |
| Buffer de recepción | `600` bytes | C (`tarea_recepcion`) | Límite de tamaño de mensaje de las hojas. Si se agranda, revisar también el de H. |
| `PIN_LED_RGB`, `BRILLO_LED`, `PIN_BOTON` | `38`, `150`, `0` | C | Placa Waveshare ESP32-S3-DEV-KIT: el WS2812 está en GPIO38 y usa **orden R-G-B**. En otras S3 suele ser GPIO48 con orden G-R-B. |

### 4.4 Solo en las hojas

| Parámetro | Valor actual | Dónde | Efecto / cuidado |
|---|---|---|---|
| `AULA`, `DEVICE_ID`, tópicos | `aula1`, `aula1-hoja` | H | Ver 2.2. `AULA` igual a la del concentrador. |
| `ESPERA_RECONEXION_MS` | `5000` | H | Margen para que vuelva el padre tras un corte breve. Más corto = cambia de padre ante cualquier interferencia. |
| `BUSQUEDAS_PLAN_B` | `3` | H | Escaneos sin ver al concentrador antes del plan B (~9 s cada uno). Menos = plan B más rápido, pero más "saltos". |
| `REVISAR_AULA_MS` | `30000` | H | En plan B, cada cuánto se fija si volvió el concentrador. |
| `ESPERA_CONEXION_MS` | `15000` | H | Tiempo que espera una conexión después de elegir padre. |
| `LIMITE_ENVIO_MS` | `5000` | H, C (G usa el mismo valor) | Tiempo máximo de un `esp_mesh_send()`. Espressif recomienda no bajar de 5 s. |
| `FALLOS_PARA_RECONECTAR` | `3` | H, C | Envíos fallidos seguidos antes de forzar la reconexión (ver 2.1, regla 12). |
| Período de datos | `2000` ms | H (`tarea_envio`) | Cada cuánto manda mediciones. |
| `PIN_LED`, `LED_ACTIVO_BAJO` | `8`, `1` | H | LED de la C3 SuperMini (activo en bajo). |
| `POTENCIA_TX` | `60` (15 dBm) | H | Pasos de 0,25 dBm (máx. 84 = 21 dBm). Define el **alcance de subida**: a 40 (10 dBm) la SuperMini solo llegaba a ~3 m con una pared. En la SuperMini, si a potencia alta no conecta (antena mal adaptada), bajar de a poco. |
| `mesh_ap.max_connection` | `1` | H | No bajar a 0 (la API no lo acepta). Las otras hojas no la eligen como padre. |
| Buffer de órdenes | `600` bytes | H (`tarea_ordenes`) | Tamaño máximo de una orden. |

### 4.5 Por placa (`sdkconfig.defaults` de cada proyecto)

| Placa | Ajustes |
|---|---|
| ESP32-C3 SuperMini | `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`, `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE` |
| Waveshare S3, módulo WROOM-1 N16R8 | flash quad 16 MB (`FLASHMODE_DIO`, `FLASHSIZE_16MB`), app grande |
| Waveshare S3, módulo WROOM-2 N32R16V | flash **octal** 32 MB (`OCT_FLASH`, `FLASHMODE_OPI`, `FLASHSIZE_32MB`), app grande |

Si se cambia `sdkconfig.defaults`, borrar `sdkconfig` y `build` antes de compilar: si no, el cambio no se aplica.

---

## 5. Problemas frecuentes

| Síntoma en el monitor de la hoja | Causa probable | Qué revisar |
|---|---|---|
| `El concentrador de aula1 no está visible` y nunca conecta | `AULA` distinta, concentrador apagado o vendor IE distinto | Sección 4.1 y `AULA` del concentrador |
| Ve nodos `malla:` pero ninguno marcado `<- concentrador` | Vendor IE (OUI o tipo) distinto entre C y H | `AURA_OUI_*` y `AURA_IE_CONCENTRADOR` |
| No ve ningún nodo `malla:` | `MESH_ID` distinto, o gateway sin arrancar | `MESH_ID` en G, C y H |
| `esp_mesh_set_parent: ESP_FAIL` | Se usó `MESH_LEAF` | Regla 7 |
| `STA is connecting, scan are not allowed!` en bucle | Se quitó el corte de reintentos de `tarea_vigia` | Restaurar `esp_mesh_set_self_organized(false,false)` antes de escanear |
| Los datos llegan con `"ts_gw":true` | La hoja todavía no recibió la hora | Normal en el primer minuto; si sigue, revisar que el gateway tenga internet |
| AURA manda una orden y el gateway dice `Nodo desconocido` | La hoja todavía no mandó ningún mensaje, o el `id` del tópico no coincide | `DEVICE_ID` y el tópico `aura/cmd/<id>` |
| La C3 conecta y se cae enseguida | Antena de la SuperMini mal adaptada | Bajar `POTENCIA_TX` de a poco (78 → 60 → 40) |
| La hoja deja de mandar a pocos metros (el tablero sigue mostrando buena señal del padre) | Poca potencia de subida: el padre no la escucha | Subir `POTENCIA_TX`; si no alcanza, ver la sección 6 |
| `[WND-RX] … timeout` repetido y no salen datos | Control de flujo trabado después de un reinicio rápido del padre | Regla 12. El vigilante lo resuelve solo en unos 20 s |
| El gateway dice `No encuentro el broker` | `broker.py` apagado, PC en otra red, o firewall | La PC tiene que estar en la misma red. Windows tiene que marcarla como *Privada* para que la regla de Python aplique. O poner `BROKER_URI` fijo |

---

## 6. Alcance

- **Señales de vida en el LED** (para diagnosticar sin PC): 2 destellos cortos al arrancar (si se repiten cada pocos
  segundos, se reinicia en bucle) y 1 destello largo al conectarse al padre. El primer mensaje después de arrancar
  trae `"reinicio"` con el motivo (`encendido`, `brownout`, `panic`, `watchdog`…).
- La hoja manda en el mensaje de sensores `"rssi"`: la señal con que **recibe** a su padre, en dBm. El tablero la muestra
  como "señal del padre": mejor que −67 es buena, entre −67 y −80 es justa, y peor que −80 está al límite.
- El RSSI mide la bajada. La **subida** depende de `POTENCIA_TX` de la hoja. Si los datos se cortan con buena señal del
  padre, el problema es la potencia o la antena de la hoja.
- La **C3 SuperMini** tiene la antena de chip mal adaptada, y por eso tiene poco alcance y es sensible a la potencia.
  Para más alcance, de más barato a más prolijo:
  1. **Modificación de antena:** soldar un alambre de 31 mm (cuarto de onda a 2,4 GHz) en la alimentación de la antena.
     Es una modificación muy difundida para esta placa y mejora mucho el alcance.
     **Probado en AURA:** con la antena y `POTENCIA_TX 60` (15 dBm), la hoja pasó de ~3 m a ~10 m a través de una vidriera,
     alimentada con powerbank. **Con la antena no subir a 19,5 dBm**: así andaba conectada a la PC pero no con cargador ni
     powerbank. Probablemente la RF del alambre se mete en la alimentación cuando la placa no tiene la masa de la PC.
  2. **Placa C3 con conector para antena externa** (u.FL/IPEX), por ejemplo la Seeed XIAO ESP32C3, que trae antena.
  3. **Para la instalación final:** módulos con antena certificada (ESP32-C3-MINI-1 o ESP32-C3-WROOM-02) en una placa propia.
- Del lado de la red, la solución de arquitectura son los **nodos de piso**: repetidores de la malla ubicados para cubrir
  pasillos y zonas lejanas. En plan B, una hoja puede colgarse de la raíz o de otro concentrador si su concentrador no la alcanza.
