# Prueba AURA: malla de 3 nodos + MQTT (contrato v4.0)

```
Hoja C3 (COM9) ──malla──> Concentrador S3 (COM11) ──malla──> Gateway S3 (COM12) ──WiFi──> Router ──> broker.py (PC)
   capa 3                    capa 2 (pantalla + botón)        capa 1 (raíz)
```

| Proyecto | Carpeta | `hw_id` | Placa | Puerto flash/monitor |
|---|---|---|---|---|
| Gateway | `Mesh3_Gateway` | `mac-90e5b1d64a94` | Waveshare ESP32-S3-DEV-KIT-NxRx, módulo ESP32-S3-WROOM-2 N32R16V (flash octal 32 MB) | COM12 (CH343) |
| Concentrador | `Mesh3_Intermedio` | `mac-dcb4d92a5878` | Waveshare ESP32-S3-DEV-KIT-NxRx, módulo ESP32-S3-WROOM-1 N16R8 (flash quad 16 MB) | COM11 (CH343) |
| Hoja | `Mesh3_Hoja` | `mac-907069add964` | ESP32-C3 SuperMini (4 MB) | COM9 |

COM13 y COM10 son el USB nativo de cada S3: sirven para flashear, pero la consola sale por el CH343.
Los tres comparten `componentes/` (trama común `aura_mesh` y biblioteca de la hoja `nodo_mesh`).

**Placas Waveshare ESP32-S3-DEV-KIT-NxRx** (gateway y concentrador tienen la misma placa con distinto módulo):
el LED RGB es un WS2812 en **GPIO38** con orden de color **R-G-B** (no el G-R-B habitual), no en el GPIO48 de las DevKitC de Espressif.
El botón BOOT es GPIO0. Pinout: [circuitstate.com](https://www.circuitstate.com/pinouts/waveshare-esp32-s3-dev-kit-nxr8-wi-fi-development-board-pinout-diagram-arduino-reference/).

> ⚠️ **No apoyar las placas una contra otra.** El 2026-10-20, con la raíz y el concentrador tocándose, la hoja no lograba
> autenticarse con la raíz (`auth expire` en bucle) y el concentrador entraba y salía de la raíz cada ~11 s
> (`reason = 209`). Separadas, todo anduvo.

## Identidad, alta y recambio de placas

- **Ningún firmware tiene MACs, ids ni aulas.** La identidad es la MAC de cada placa: `hw_id = mac-<12 hex>`. Todas las hojas
  de un mismo tipo llevan **el mismo binario**.
- El **aula** se guarda en NVS y la asigna AURA con `set_config {"aula":"aula1"}`, tanto a la hoja como al concentrador.
- **Placa nueva:** se graba y se enciende. Sin aula, la hoja se cuelga de la raíz (o de cualquier concentrador) y manda directo;
  AURA la ve **pendiente** (cuarentena). Al aceptarla le asigna el dispositivo y el aula, y la hoja se pasa a su concentrador.
- **Recambio:** misma placa, mismo firmware, y en AURA se le asigna la placa nueva al dispositivo viejo (conserva el historial).

## Tópicos (árbol `hw/` del contrato)

| Sentido | Tópico | Ejemplo |
|---|---|---|
| Hoja → AURA | `hw/<hw_id>/data` | `{"values":{"temp_c":24.5,"hum_pct":50.5,"presencia":false,"led":0},"ingest_id":"9f75…","ts":"2026-10-20T04:18:21Z","adaptador":"mac-90e5b1d64a94"}` |
| AURA → hoja | `hw/<hw_id>/ack` | `{"ingest_id":"9f75…","resultado":"persistido"}` |
| Nodo → AURA (retenido) | `hw/<hw_id>/status` | `{"status":"online","details":{"config":{"aula":"aula1","intervalo_s":10},"pendientes":0,"descartadas":0,"alimentacion":"red","modo":"aula","rssi":-19,"capa":3,"evento":"reporte","transporte":"mesh"}}` |
| AURA → nodo | `hw/<hw_id>/command` | `{"command":"led","params":{"encendido":true},"command_id":"c-1"}` |
| Nodo → AURA | `hw/<hw_id>/response` | `transmitido` → `recibido` → `aplicado` / `rechazado` (con `motivo`) |
| Gateway (retenido, LWT) | `hw/mac-90e5b1d64a94/status` | red, canal, nodos, contadores (`publicadas`, `confirmadas`, `lotes`…), `hora_valida`, `uptime_s` |

Dentro de la malla viaja la **trama binaria v3** (`componentes/aura_mesh/include/aura_trama.h`, igual a `comun/protocolo_aura.h`),
más dos tipos internos: **LOTE** (el concentrador junta las tramas de sus hojas, cada una con la MAC de la hoja) y **HORA**.

## Modo aula y plan B de la hoja

```
MODO AULA (normal)    Hoja ──1 trama──> Concentrador ──> pantalla + LOTE ──> Raíz ──> hw/<hoja>/...
                      AURA ──> Raíz ──(a la MAC de la hoja; la malla enruta)──> Concentrador ──> Hoja

PLAN B / SIN AULA     Hoja ──────────────> Raíz (u otro concentrador, nunca otra hoja) ──> hw/<hoja>/...
```

- **Modo aula:** la hoja se cuelga de su concentrador y le manda cada dato **una sola vez**. El concentrador lo muestra y lo
  reenvía a la raíz dentro de un LOTE (junta lo que llega en 50 ms). Nada viaja dos veces.
- **Plan B:** si pierde al concentrador, le da 5 s para volver y después lo busca; tras 3 escaneos sin verlo se cuelga de la
  **raíz o de otro concentrador** y manda directo. Cada 30 s revisa si volvió su concentrador.
- **Si le cambian el aula al concentrador**, este suelta a sus hojas; cada hoja revisa además cada 30 s que su padre siga
  anunciando su aula. Una hoja de otra aula pasa a plan B en ~5 s (cambiando de padre sin desconectarse: si se desconecta,
  la pila la vuelve a colgar del mismo padre en 0,2 s).
- El concentrador **solo muestra** las hojas que informan su misma aula, pero reenvía todo (para que AURA no pierda datos).

## Entrega garantizada y hora

- La hoja guarda cada medición en una **cola en RAM** (400 lugares, sin escribir la flash) con su `ingest_id` antes de
  enviarla, y la saca solo con la CONFIRMACIÓN que manda la raíz cuando llega el `ack` de AURA. Sin `ack` reintenta
  (15 s, 30 s, 1 min… 5 min, con azar). Lo pendiente se pierde si la hoja se reinicia.
- **Eventos** (resultado, reporte, alerta): la raíz los confirma (`EVENTO_OK`) recién cuando los publicó; hasta entonces
  el nodo los reintenta. **Cuando vuelve el broker**, la raíz pide reporte a todos (`REPORTAR`): cada nodo publica su
  estado actual y la hoja manda lo pendiente enseguida.
- La raíz toma la hora por SNTP. La hoja la pide al conectarse (la contesta el concentrador si la tiene) y la ajusta con cada
  confirmación. Una medición tomada sin hora se fecha con el uptime al enviarla, si es del mismo arranque.
- El ESP32 no tiene pin de batería para el RTC: para conservar la hora tras un corte de luz hace falta un RTC con pila (DS3231).

## Broker de banco (`broker.py`) y tablero

- Broker MQTT mínimo con **mensajes retenidos** y **Last Will**.
- **AURA falsa:** contesta cada `data` con su `ack` (`persistido`, o `duplicado` si ya lo vio). El modo se cambia en el tablero:
  `ok`, `sin_ack` (base caída), `cuarentena`, `rechazado`, `apagada` (con el backend real, o arrancar con `--sin-aura`).
- **Tablero** (http://localhost:8080): una tarjeta por placa (`hw_id`) con su rol, estado (online, offline, **pendiente**
  si no tiene aula), aula, valores y botones: LED, **asignar aula** e intervalo. Abajo, todos los mensajes.
- `python led_blink.py <hw_id>`: prende y apaga el LED cada 1 s y mide el tiempo hasta `aplicado`.

## Cómo probar

1. **Firewall (una sola vez, como administrador):** permitir `python.exe` en redes **privadas**.
2. `python broker.py` en esta carpeta → abrir **http://localhost:8080**.
3. Encender las 3 placas (el orden no importa) **separadas entre sí**.
4. En el tablero, asignar el aula al concentrador y a la hoja (`aula1`). La hoja pasa de `sin_aula` a `aula` sola.

### Desconectar y volver a armar todo

Los firmwares y las aulas quedan en flash: **no hay que volver a flashear ni a asignar nada**. Alimentar las placas,
levantar `broker.py` y esperar ~1 minuto. (Lo que una hoja tenía sin confirmar se pierde si se la desenchufa.)

### En otra red WiFi

1. Agregar la red a `REDES_WIFI` en `Mesh3_Gateway/main/credenciales.h` (en orden de preferencia) y **regrabar solo el gateway**.
2. La red tiene que tener **2,4 GHz**, sin portal de login ni aislamiento de clientes.
3. Windows tiene que marcar la red como **Privada**, y la PC tiene que estar en la misma red que el gateway.

## Notas de diseño

- La raíz busca el router antes de armar la malla y usa su canal de 2,4 GHz. Concentrador y hoja arrancan con canal 0.
- **País AR** (canales 1–13) en los tres: con el país por defecto el router en el canal 12 o 13 no se ve.
- **Solo el gateway depende del WiFi.** Concentrador y hoja usan un SSID ficticio fijo (`aura-sin-router`).
- **Descubrimiento del broker:** primero el último que anduvo en esa red (NVS), después broadcast UDP 18830.
- **MQTT:** el gateway publica con `esp_mqtt_client_publish` (sale enseguida) y atiende `ack` y comandos en una tarea aparte.
  Se probó `esp_mqtt_client_enqueue` (no bloquea), pero el cliente vacía esa bandeja recién al volver de esperar datos del
  broker (hasta 1 s): cada mensaje se demoraba 0–1 s. Con `publish`, la ida y vuelta de un comando es de **29–51 ms** (prom. 35).
- **Latencia en el aula:** la hoja atiende un comando apenas llega, y el concentrador manda enseguida resultados, reportes y
  alertas; solo la telemetría se junta 50 ms en un LOTE.
- **Vigilante de envío:** tras 3 envíos fallidos el nodo fuerza la reconexión (evita el `[WND-RX] timeout` de ~6 min).
- **La hoja no puede ser `MESH_LEAF` en IDF 6.1:** `esp_mesh_set_parent(..., MESH_LEAF, ...)` devuelve `ESP_FAIL` al instante
  (probado de nuevo con la radio sana), y pasar a `MESH_LEAF` ya conectada desconecta en bucle. Se une como `MESH_NODE`, sube el
  beacon de su AP de 300 a 60 000 TU (~200 veces menos beacons) y rechaza a cualquier hijo.

## Resultado con el contrato v4.0 (20/10/2026)

- ✅ Gateway: red `Personal-419` de la lista, broker guardado en 0,1 s, publica en `hw/`, status propio con LWT.
- ✅ Hoja sin aula: entra directo a la raíz y sus datos llegan como `hw/mac-907069add964/data` (pendiente para AURA).
- ✅ Cola (entonces en flash; desde la tarde, en RAM): al conectarse vació 96 mediciones guardadas, todas con `ts`.
- ✅ Asignación de aula desde el tablero: `transmitido` → `recibido` → `aplicado` con la config vigente, en hoja y concentrador.
  La hoja pasó sola de directo a su concentrador (capa 3), y los datos subieron en LOTE (55 lotes, 0 tramas inválidas).
- ✅ Comandos a través del concentrador: LED encendido/apagado `aplicado` (29–51 ms ida y vuelta con `led_blink.py`, 22 de 22);
  comando inventado → `rechazado comando_desconocido`.
- ✅ Plan B: con el concentrador callado, la hoja pasó a la raíz en ~35 s y mandó las 5 mediciones guardadas, sin pérdidas.
  Volvió sola al concentrador ~20 s después de que reapareció. El concentrador recuperó su aula de NVS.
- ✅ Offline inferido: con la hoja callada, `status offline` (`sin_tramas`) a los ~30 s (3 × 10 s).
- ✅ AURA caída (`sin_ack`): la hoja acumuló 11 mediciones con reintentos a 15 s, 30 s y 60 s; al volver AURA las mandó todas.
- ✅ Cambio de aula del concentrador (`aula1` → `aula11`) con la hoja en `aula1`: la hoja pasó a plan B en ~5 s y el
  concentrador dejó de mostrarla.
- ✅ Botón local del concentrador: actúa solo sobre hojas de su misma aula (probado por Milton).
- ✅ Broker caído justo cuando cambia el LED: la medición con el `led` nuevo quedó en la hoja y llegó al volver el broker,
  con la hora del cambio. AURA terminó con el estado real.
- ✅ Broker caído 40 s con la hoja reconectándose (cola en RAM y eventos confirmados): el reporte se reintentó durante el
  corte; al volver, la raíz pidió reporte a los 2 nodos, se publicaron los reportes y las 5 mediciones pendientes salieron
  en el acto (~250 ms cada una).
- ⏳ Falta: una prueba larga (horas).

## Resultados anteriores (formato JSON propio, 19–20/10/2026)

- La malla funcionó toda una noche sin reinicios (~8 h, n = 14 181, sin mensajes perdidos).
- LED ida y vuelta con `led_blink.py`: 14 a 27 ms en la red de la casa; 77–242 ms en la red de la facultad (IngComputacion, canal 1).
- Alcance de la hoja (C3 SuperMini): ~3 m con una pared a 10 dBm; ~10 m a través de una vidriera con antena de alambre de 31 mm
  y 15 dBm. Con la antena y 19,5 dBm andaba solo conectada a la PC.
