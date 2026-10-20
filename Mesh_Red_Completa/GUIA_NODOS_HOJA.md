# AURA – Guía para desarrollar nodos hoja

Esta guía es para quienes programen **nodos hoja** (sensores y actuadores de un aula) que se conectan a la malla
ESP-WIFI-MESH de AURA. Todo es **ESP-IDF nativo** (sin Arduino) y cumple el **contrato MQTT v4.0**
(`docs/CONTRATO_MQTT.md` del repositorio `aura-firmware`).

Una hoja se programa sobre la biblioteca **`nodo_mesh`** (`componentes/nodo_mesh`): el grupo escribe **qué mide,
qué parámetros tiene y qué comandos acepta**. La conexión, el aula, la cola, la confirmación de AURA, los eventos
y la hora ya están resueltos en la biblioteca. El ejemplo de referencia es `Mesh3_Hoja/main/main.c` (ESP32-C3 SuperMini):
para una hoja nueva conviene **copiar ese proyecto** y cambiar solo lo que se indica acá.

---

## 1. Cómo funciona la red

```
                         MQTT: árbol hw/<hw_id>/... (AURA)
                                   ▲
                                   │ WiFi del router (2,4 GHz)
                         ┌─────────┴─────────┐
                         │  GATEWAY (raíz)   │  capa 1 · ESP32-S3 · único nodo con IP
                         └─────────┬─────────┘
                    malla          │
            ┌──────────────────────┴───────────────────┐
   ┌────────┴─────────┐                       ┌─────────┴────────┐
   │ CONCENTRADOR     │  capa 2 · ESP32-S3    │ CONCENTRADOR     │
   │ aula1            │  pantalla + botón     │ aula2            │
   └───┬─────────┬────┘                       └───┬──────────────┘
       │         │                                │
   ┌───┴───┐ ┌───┴───┐                        ┌───┴───┐
   │ hoja  │ │ hoja  │  capa 3 · ESP32-C3     │ hoja  │
   └───────┘ └───────┘                        └───────┘
```

| Rol | Qué hace |
|---|---|
| **Gateway** | Raíz única de la malla y **adaptador** del contrato: publica todo en `hw/<hw_id>/...` con el `hw_id` de la placa que lo midió, baja los comandos y los `ack` de AURA, toma la hora de internet y la reparte. |
| **Concentrador** | Uno por aula. Padre de las hojas de su aula: **ve** sus datos (pantalla), puede filtrarlos, y los reenvía a la raíz **una sola vez**, juntos en un LOTE. Tiene mando local (botón). |
| **Hoja** | Sensores y actuadores. Se cuelga del concentrador de **su** aula. |

### 1.1 Identidad: la MAC de la placa

Cada placa se identifica con su MAC de fábrica: `hw_id = mac-<12 hex>` (por ejemplo `mac-907069add964`).
**No hay MACs, ids ni aulas escritos en ningún firmware.** Todas las hojas de un mismo tipo llevan **el mismo binario**.

- **Alta de una placa nueva:** se graba el firmware y se enciende. Como todavía no tiene aula, se cuelga de la raíz
  (o de cualquier concentrador) y manda directo. AURA no la conoce y la deja **pendiente** (cuarentena: guarda los
  datos y los confirma igual). Al aceptarla, AURA le asigna el dispositivo y le manda
  `set_config {"aula":"aula1"}`. La hoja lo guarda en NVS y se pasa a su concentrador.
- **Recambio de una placa quemada:** se graba el mismo firmware en la nueva, y en AURA se le asigna esa placa
  (aparece pendiente) al dispositivo viejo, con su aula. El dispositivo conserva su historial.
- El concentrador también recibe su aula con `set_config` (en NVS); sin aula no anuncia nada.

### 1.2 Cómo encuentra la hoja a su concentrador

Cada concentrador con aula anuncia en su beacon WiFi un *vendor IE* de AURA: "soy el concentrador de `aula1`",
con su dirección en la malla. La hoja escanea y elige como padre al que anuncia **su** aula. El padre se elige a
mano (no por señal) para que un sensor del aula 2 no termine en el concentrador del aula 1.

| Modo (`modo` en el status) | Cuándo | Padre | A quién manda |
|---|---|---|---|
| `sin_aula` | AURA todavía no le asignó aula | La raíz u otro concentrador | Directo a la raíz |
| `aula` (normal) | Ve a su concentrador | Su concentrador | **Solo** al concentrador, que muestra y reenvía |
| `directo` (plan B) | No ve a su concentrador | La raíz u **otro concentrador** (nunca otra hoja) | Directo a la raíz |

- **Cada dato viaja una sola vez.** En modo aula la hoja se lo manda al concentrador, que lo reenvía a la raíz dentro
  de un LOTE con la MAC de la hoja: nada se duplica en el canal.
- Si la hoja pierde al concentrador, le da 5 s para volver; si no, lo busca, y tras 3 escaneos sin verlo pasa a plan B
  (~35 s en total, probado). En plan B revisa cada 30 s si volvió, y vuelve sola (probado: ~20 s).
- **AURA no nota nada:** el `hw_id` es el de la hoja en cualquier modo.
- Lo que AURA le manda a la hoja (comandos, confirmaciones) va a su MAC y la malla lo enruta sola.

### 1.3 Entrega garantizada

- **Mediciones:** cada una entra a una **cola en RAM** con su `ingest_id` (UUID) **antes** del primer envío, y sale solo
  cuando la raíz le manda la CONFIRMACIÓN, que llega cuando AURA guardó la muestra (`ack`). Si AURA, el broker, el WiFi
  o el concentrador se caen, la hoja guarda y reintenta (15 s, 30 s, 1 min... hasta 5 min, **con azar** para que después
  de un corte no reintenten todas juntas). La cola tiene lugar para **400 mediciones** (~66 min de corte midiendo cada
  10 s); llena, se pierde la más vieja (`descartadas` en el status).
- **Híbrido RAM + flash:** en marcha normal la cola vive en RAM y **la flash no se escribe**. Si una medición pasa
  **5 minutos** sin confirmar (hay un corte de verdad), se respalda en la partición `cola` de la flash, con su hora.
  Así sobrevive a un reinicio o a un corte de luz de la hoja: al arrancar, lo pendiente vuelve a la cola, primero que
  todo. Al confirmarse, en la flash solo se cambia un byte de estado (de 0xFF a 0x00, sin borrar); un sector se borra
  recién cuando el registro circular da la vuelta completa, así que el desgaste es despreciable.
- **Eventos** (resultado de un comando, reporte, alertas): se reintentan (2 s, 4 s... hasta 30 s, con azar) hasta que la
  raíz confirma que los **publicó** (`EVENTO_OK`). Un reporte nuevo reemplaza al viejo que todavía no salió.
- **Cuando vuelve el broker**, la raíz les pide reporte a todos los nodos (`REPORTAR`): cada hoja publica su estado
  actual y manda lo pendiente enseguida (en un momento al azar de 0 a 3 s), sin esperar la espera de reintento.
- Así AURA termina con el **estado real** después de cualquier corte: por ejemplo, si el LED cambia con el broker caído,
  la medición con el `led` nuevo sale en cuanto vuelve, con la hora del cambio.

### 1.4 Hora

La raíz toma la hora de internet (SNTP). La hoja la pide al conectarse (`HORA`; si el concentrador la tiene, contesta
él) y la ajusta con cada confirmación. Cada medición sale con su `ts`. Una medición tomada **antes** de tener hora se
fecha al enviarla con el uptime. Para conservar la hora tras un corte de luz hace falta un RTC con pila (DS3231).

---

## 2. Cómo se programa una hoja

### 2.1 Lo mínimo

```c
#include "nodo_mesh.h"

static bool aplicar_config(const cJSON *params, char *motivo, size_t tam);   /* set_config */
static void describir_config(cJSON *config);                                 /* config vigente */
static bool ejecutar_comando(const char *comando, const cJSON *params, bool local,
                             char *motivo, size_t tam);                      /* comandos propios */

void app_main(void)
{
    nvs_flash_init();
    nodo_mesh_config_t cfg = {
        .mesh_id = {MESH_ID_BYTES},           /* de config_local.h */
        .mesh_clave = MESH_AP_PASS,
        .potencia_tx = 60,                    /* 15 dBm; 0 = la de fábrica */
        .alimentacion = "red",
        .aplicar_config = aplicar_config,
        .describir_config = describir_config,
        .ejecutar_comando = ejecutar_comando,
    };
    nodo_mesh_iniciar(&cfg);
    /* ... y en una tarea propia, cada intervalo: */
    cJSON *v = cJSON_CreateObject();
    cJSON_AddNumberToObject(v, "temp_c", 24.5);
    nodo_mesh_medicion(v);                    /* entra a la cola y sale cuando se pueda */
    cJSON_Delete(v);
}
```

| Función | Para qué |
|---|---|
| `nodo_mesh_iniciar(&cfg)` | Una vez, después de `nvs_flash_init()`. Arranca WiFi, malla y sus tareas. |
| `nodo_mesh_medicion(values)` | Guarda una medición en la cola. `values` lleva **solo mediciones válidas** (ver 2.3). |
| `nodo_mesh_sonda(campo, ok, motivo)` | En cada medición, el estado de cada sonda: manda la alerta `sensor` **solo cuando cambia**. |
| `nodo_mesh_alerta(tipo, severity, message, details)` | Otra alerta: `hw/<hw_id>/alerts/<tipo>`. |
| `nodo_mesh_reportar()` | Manda el reporte de estado (la biblioteca ya lo manda al conectarse y tras un `set_config`). |
| `nodo_mesh_conectada()`, `nodo_mesh_aula()`, `nodo_mesh_pendientes()` | Estado, por ejemplo para un LED. |

### 2.2 Reglas

1. **Parámetros de malla idénticos** al resto de la red: `MESH_ID_BYTES` y `MESH_AP_PASS` en `main/config_local.h`
   (no se sube al repositorio). Si uno difiere, la hoja no se une.
2. **Nada de MACs, ids ni aula en el código.** La identidad es la placa y el aula la asigna AURA.
3. **`set_config` valida todo o no aplica nada** (contrato §3.3): `aplicar_config` primero revisa **todos** los
   parámetros y solo si todos son válidos los aplica y los guarda en NVS. Tiene que **ignorar `aula`**, que la maneja
   la biblioteca. Si algo no valida, devuelve `false` con el motivo (`intervalo_s_fuera_de_rango`, `parametro_desconocido`…).
4. **`describir_config` incluye `intervalo_s`**: con eso la raíz publica `offline` si la hoja pasa 3 intervalos sin hablar.
5. **Comandos:** `ejecutar_comando` devuelve `true` solo si **ejecutó**. Lo que AURA ve como `aplicado` sale de ahí.
   `local = true` es una orden del botón del concentrador: no se le contesta a AURA, pero el estado nuevo tiene que
   aparecer en la medición siguiente (el ejemplo mide enseguida).
6. **No usar `esp_mesh_*` ni `esp_wifi_*` directamente**: la biblioteca maneja la radio.
7. **JSON con cJSON**: liberar lo que se crea (`cJSON_Delete`). `nodo_mesh_medicion` no se queda con el objeto.
8. **Números con decimales como `double` redondeados**: con `float`, cJSON escribe `24.100000381`.
9. **Tamaños:** `values` hasta **160 bytes** de JSON; un comando, hasta 180 bytes.

### 2.3 Qué va en `values` (contrato §3.1)

- **Solo mediciones** (y el estado de los actuadores). Nombres en minúsculas con la unidad: `temp_c`, `hum_pct`, `co2_ppm`.
- **Una sonda que falla no aparece**: ni centinela (`-127`, `85`), ni `null`. Se informa con `nodo_mesh_sonda()`.
- **Nada del enlace**: RSSI, capa, cola o batería van en el **status**, y eso lo arma la biblioteca.

### 2.4 Lo que publica la raíz por cada hoja

| Tópico | Cuándo | Ejemplo |
|---|---|---|
| `hw/<hw_id>/data` | Cada medición (los reintentos traen el mismo `ingest_id`) | `{"values":{"temp_c":24.5,"led":0},"ingest_id":"9f75…","ts":"2026-10-20T04:18:21Z","adaptador":"mac-90e5b1d64a94"}` |
| `hw/<hw_id>/status` (retenido) | Al conectarse, tras un `set_config` y al volver | `{"status":"online","details":{"config":{"aula":"aula1","intervalo_s":10},"pendientes":0,"descartadas":0,"alimentacion":"red","modo":"aula","rssi":-19,"capa":3,"evento":"reporte","transporte":"mesh"}}` |
| `hw/<hw_id>/status` (retenido) | 3 intervalos sin datos | `{"status":"offline","details":{"motivo":"sin_tramas"}}` |
| `hw/<hw_id>/response` | Cada comando: `transmitido`, `recibido`, `aplicado`/`rechazado` | `{"status":"aplicado","details":{"command_id":"c-1","config":{…}}}` |
| `hw/<hw_id>/alerts/<tipo>` | Alertas de la hoja | `{"severity":"high","message":"Sonda temp_c con falla","details":{"campo":"temp_c","motivo":"sin_respuesta"}}` |

### 2.5 Comandos que recibe (de AURA por `hw/<hw_id>/command`)

| Comando | Lo atiende | Ejemplo |
|---|---|---|
| `set_config` | biblioteca (`aula`) + `aplicar_config` (el resto) | `{"command":"set_config","params":{"aula":"aula1","intervalo_s":30},"command_id":"c-7"}` |
| propios | `ejecutar_comando` | `{"command":"led","params":{"encendido":true},"command_id":"c-8"}` |

Un comando desconocido se contesta `rechazado` con `comando_desconocido`. Ningún comando queda sin respuesta: si la
hoja no contesta en 30 s, la raíz publica `rechazado` con `sin_resultado_del_nodo`.

### 2.6 Cómo agregar un actuador (ejemplo: ventana)

```c
static bool ejecutar_comando(const char *comando, const cJSON *params, bool local, char *motivo, size_t tam)
{
    if (strcmp(comando, "ventana") == 0) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(params, "apertura_pct");
        if (!cJSON_IsNumber(p) || p->valuedouble < 0 || p->valuedouble > 100) {
            snprintf(motivo, tam, "apertura_fuera_de_rango");
            return false;
        }
        ventana_mover(p->valuedouble);       /* con fines de carrera y tiempo máximo de motor */
        medir_ya = true;                     /* la medición siguiente lleva la apertura REAL */
        return true;
    }
    snprintf(motivo, tam, "comando_desconocido");
    return false;
}
```

**Seguridades en la propia hoja** (fines de carrera, tiempo máximo de motor, posición segura si pierde la
comunicación mucho tiempo): no dependen de AURA ni del concentrador.

---

## 3. Checklist para una hoja nueva

- [ ] Copié `Mesh3_Hoja` a una carpeta nueva **al lado de `componentes/`** (el `CMakeLists.txt` la busca en `../componentes`), sin espacios ni tildes, fuera de Google Drive/OneDrive.
- [ ] Cambié el nombre en `project(...)` del `CMakeLists.txt` raíz.
- [ ] Copié `main/config_local.ejemplo.h` como `main/config_local.h` y completé el ID y la clave de la mesh (los da la cátedra; **no se sube**).
- [ ] Ajusté `sdkconfig.defaults` a la placa (target, flash), mantuve `partitions.csv` con la partición `cola` y borré `sdkconfig` y `build`.
- [ ] Escribí la medición, `aplicar_config`, `describir_config` (con `intervalo_s`) y `ejecutar_comando`.
- [ ] Ajusté pines y `potencia_tx` a la placa.
- [ ] Probé con el broker de banco (`Broker_MQTT/broker.py`): la placa aparece **pendiente** en el tablero, le asigné el aula, se pasó a `modo: aula` y llegan los `data` con `ts`.
- [ ] Probé un `set_config` inválido: `rechazado` con motivo, y la configuración no cambió.
- [ ] Probé el plan B apagando el concentrador: pasa a `directo`, no pierde datos y vuelve sola a `aula`.

---

## 4. Parámetros de red y dónde replicarlos

**G** = `Mesh3_Gateway/main/main.c` · **Gc** = `Mesh3_Gateway/main/credenciales.h` ·
**C** = `Mesh3_Intermedio/main/main.c` (concentrador) · **NM** = `componentes/nodo_mesh/nodo_mesh.c` (toda hoja) ·
**H** = `Mesh3_Hoja/main/main.c` (la hoja de ejemplo) · **AM** = `componentes/aura_mesh/include/` (trama común) ·
**PC** = `Broker_MQTT/` (broker, tablero y scripts de prueba)

Después de cambiar un parámetro hay que **recompilar y grabar todos los firmwares donde aparece**. Un cambio en
**NM** o **AM** afecta a todas las hojas (y en AM, también al gateway y a los concentradores).

### 4.1 Comunes a toda la malla: deben ser IDÉNTICOS

| Parámetro | Valor actual | Dónde | Efecto / cuidado |
|---|---|---|---|
| `MESH_ID_BYTES` | (en `config_local.h`) | G, C, H — `main/config_local.h` | Identifica la red. Nodos con otro ID no se ven. |
| `MESH_AP_PASS` | (en `config_local.h`) | G, C, H — `main/config_local.h` | Clave WPA2 entre nodos (mínimo 8 caracteres). La da la cátedra. |
| `MESH_MAX_CAPAS` | `6` | G, C, NM | Profundidad máxima del árbol. |
| `ROUTER_FICTICIO` | `aura-sin-router` | C, NM | La API exige un SSID de router, pero estos nodos **nunca** se conectan al router. Fijo: **no se cambia al mudar la red**. |
| Vendor IE (`AURA_OUI_0..2`, `AURA_IE_CONCENTRADOR`) | `02:A5:52`, tipo `1` | AM (`aura_util.h`) | Es como la hoja reconoce a los concentradores. |
| Trama (`AURA_PROTO_VERSION`, tipos, `AURA_PAYLOAD_MAX`) | v3, 180 B | AM (`aura_trama.h`) | Igual a `comun/protocolo_aura.h` más los tipos internos HORA (9), LOTE (10), EVENTO_OK (11) y REPORTAR (12). Los tipos **no se renumeran**, solo se agregan. Una trama de otra versión se descarta y se cuenta (`version_distinta`). |
| País | `AR` (canales 1–13) | G, C, NM | Sin esto, con el router en el canal 12 o 13 la malla no arranca. |

### 4.2 Solo en el gateway

> **El gateway es el único nodo que depende del WiFi.** Para cambiar o agregar una red se edita `credenciales.h` y se
> regraba **solo el gateway**. Concentradores y hojas no se tocan.

| Parámetro | Valor actual | Dónde | Efecto / cuidado |
|---|---|---|---|
| `REDES_WIFI` | `Personal-419`, `IngComputacion` (claves en el archivo) | Gc | Lista `{"SSID", "clave"}` en orden de preferencia (2,4 GHz, sin portal de login ni aislamiento de clientes). Usa la primera al alcance; si queda 5 min sin router (`REINICIO_SIN_ROUTER_MS`), se reinicia y empieza por la siguiente. |
| `BROKER_URI` | `""` | Gc | Vacío = prueba el **último broker que le anduvo en esa red** (NVS) y, si falla 3 veces, lo busca por broadcast UDP. `"mqtt://IP:puerto"` = fijo (Mosquitto, servidor de AURA). |
| `MQTT_USUARIO`, `MQTT_CLAVE` | vacíos (anónimo) | Gc | El broker de aura-app no acepta anónimos: usuario `raiz` y la clave de su `.env`. |
| `PUERTO_DESCUBRIMIENTO` | `18830` (UDP) | G **y** PC | Tiene que coincidir con `broker.py`. |
| `ESPERA_RESULTADO_MS` | `30000` | G | Comando sin resultado del nodo → `rechazado` `sin_resultado_del_nodo`. |
| `INTERVALO_STATUS_MS` | `60000` | G | Cada cuánto publica su propio status. |
| `NODOS_CAP` | `64` | G | Nodos que sigue para offline y repetidos. |
| `CANAL_SIN_ROUTER` | `6` | G | Canal si no ve ninguna red (la malla anda, sin MQTT). |
| `SERVIDOR_NTP`, `ZONA_HORARIA` | `pool.ntp.org`, UTC-3 | G | La zona solo afecta los logs: `ts` siempre en UTC. |

### 4.3 Solo en el concentrador

| Parámetro | Valor actual | Dónde | Efecto / cuidado |
|---|---|---|---|
| Aula | NVS (la asigna AURA) | — | `set_config {"aula":"…"}` al `hw_id` del concentrador. |
| `ESPERA_LOTE_MS` | `50` | C | Cuánto junta datos de sus hojas antes de mandarlos en un LOTE. Más alto = menos paquetes, más demora. |
| `INTERVALO_REPORTE_S` | `60` | C | Su propio status (offline a los 3 intervalos). |
| `filtrar()` | deja pasar todo | C | Lógica local del aula. Ojo: una telemetría que no sigue nunca se confirma y la hoja la reintenta. |
| `MESH_MAX_HIJOS` | `6` | C | Hojas directas del aula (máximo 10). |
| `PIN_LED_RGB`, `BRILLO_LED`, `PIN_BOTON` | `38`, `150`, `0` | C | Waveshare ESP32-S3-DEV-KIT: WS2812 en GPIO38, orden **R-G-B**. |

### 4.4 Solo en las hojas

| Parámetro | Valor actual | Dónde | Efecto / cuidado |
|---|---|---|---|
| Aula | NVS (la asigna AURA) | — | Sin aula la hoja anda en `sin_aula` (directo a la raíz). |
| `intervalo_s` | `10` (2 a 3600) | H, NVS | Cada cuánto mide. Se cambia con `set_config`. |
| `ESPERA_RECONEXION_MS` | `5000` | NM | Margen para que vuelva el padre tras un corte breve. |
| `BUSQUEDAS_PLAN_B` | `3` | NM | Escaneos sin ver al concentrador antes del plan B. |
| `REVISAR_AULA_MS` | `30000` | NM | En plan B, cada cuánto se fija si volvió. Al recibir un aula nueva revisa 5 veces seguidas, cada 3 s. |
| `LIMITE_ENVIO_MS`, `FALLOS_PARA_RECONECTAR` | `5000`, `3` | NM, C | Vigilante de envío: tras 3 envíos fallidos fuerza la reconexión (evita el `[WND-RX] timeout`). |
| `BEACON_HOJA_TU` | `60000` | NM | El AP de la hoja casi no emite beacons (ver 4.6). |
| `COLA_CAPACIDAD` | `400` (~75 KB de RAM) | NM (`cola.h`) | Mediciones que aguanta sin confirmación. Llena: se pierden las más viejas (`descartadas`). Si no hay RAM, usa la mitad. |
| `COLA_A_FLASH_S` | `300` (5 min) | NM (`cola.h`) | Sin confirmar tanto tiempo, la medición se respalda en la flash. |
| Partición `cola` | 256 KB (1024 registros de 256 B) | `partitions.csv` | Respaldo de lo que no se confirmó tras un corte. |
| `EVENTO_ESPERA_MIN_MS`, `EVENTO_ESPERA_MAX_MS` | `2000`, `30000` | NM | Reintento de eventos sin `EVENTO_OK`. |
| `potencia_tx` | `60` (15 dBm) | H | Pasos de 0,25 dBm. Ver la sección 6. |
| `PIN_LED`, `LED_ACTIVO_BAJO` | `8`, `1` | H | LED de la C3 SuperMini (activo en bajo). |

### 4.5 Por placa (`sdkconfig.defaults` de cada proyecto)

| Placa | Ajustes |
|---|---|
| ESP32-C3 SuperMini (hoja) | `FLASHSIZE_4MB`, `PARTITION_TABLE_CUSTOM` con `partitions.csv` (app 1,5 MB + `cola` 256 KB) |
| Waveshare S3, módulo WROOM-1 N16R8 (concentrador) | flash quad 16 MB (`FLASHMODE_DIO`, `FLASHSIZE_16MB`), app grande |
| Waveshare S3, módulo WROOM-2 N32R16V (gateway) | flash **octal** 32 MB (`OCT_FLASH`, `FLASHMODE_OPI`, `FLASHSIZE_32MB`), app grande |

Si se cambia `sdkconfig.defaults`, borrar `sdkconfig` y `build` antes de compilar.

### 4.6 Por qué la hoja no es `MESH_LEAF`

En IDF 6.1, `esp_mesh_set_parent()` con `MESH_LEAF` devuelve `ESP_FAIL` en el acto, y pasar a `MESH_LEAF` ya conectada
hace que la pila se desconecte y reconecte en bucle (probado el 2026-10-20). Por eso la biblioteca intenta `MESH_LEAF`
una vez (por si otra versión de IDF lo acepta) y, si no, se une como `MESH_NODE` y:
- sube el intervalo de beacon de su AP de 300 a **60 000 TU** (~200 veces menos beacons en el canal del campus);
- **rechaza** a cualquier nodo que intente colgarse de ella.

---

## 5. Problemas frecuentes

| Síntoma | Causa probable | Qué revisar |
|---|---|---|
| La hoja queda en `sin_aula` | AURA todavía no le asignó aula | `set_config {"aula":…}` desde AURA o el tablero |
| `El concentrador de aula1 no está visible` y pasa a plan B | Concentrador apagado, sin aula, o con otra aula | El status del concentrador: `config.aula` |
| No ve ningún nodo de la malla | `MESH_ID` distinto, o gateway sin arrancar | `config_local.h` en G, C y H |
| `auth expire` en bucle, o el concentrador entra y sale de la raíz cada ~11 s (`reason = 209`) | **Placas tocándose o muy juntas** | Separarlas. Pasó en el banco el 2026-10-20: con las placas separadas anduvo todo |
| `STA is connecting, scan are not allowed!` en bucle | Se tocó el corte de reintentos de `tarea_vigia` | Restaurar `esp_mesh_set_self_organized(false,false)` antes de escanear |
| Los `data` llegan sin `ts` | La hoja todavía no tiene hora y la medición es de un arranque anterior | Normal tras un corte sin red; si sigue, revisar que el gateway tenga internet |
| `rechazado` `nodo_no_alcanzable` | La hoja no está en la malla | Que esté encendida y conectada |
| `rechazado` `sin_resultado_del_nodo` | La hoja recibió el comando pero no contestó en 30 s | Que `ejecutar_comando` no se bloquee |
| La cola crece y no baja | AURA no confirma (sin backend, o la AURA falsa en `sin_ack`) | El modo de la AURA falsa en el tablero |
| `descartadas` o `eventos_perdidos` en el status | Un corte más largo de lo que entra en la cola (400 mediciones, 8 eventos) | Normal tras cortes largos; con intervalos cortos, subir `COLA_CAPACIDAD` |
| `[WND-RX] … timeout` repetido | Control de flujo trabado tras un reinicio rápido del padre | El vigilante lo resuelve solo en ~20 s |
| El gateway dice `No encuentro el broker` | `broker.py` apagado, PC en otra red, o firewall | Misma red, Windows con la red como *Privada*, o `BROKER_URI` fijo |
| La C3 conecta y se cae enseguida | Antena de la SuperMini mal adaptada | Sección 6 |

---

## 6. Alcance

- El status de la hoja trae `rssi` (cómo **recibe** a su padre, en dBm) y `capa`: mejor que −67 es buena, entre −67 y
  −80 justa, peor que −80 al límite. El RSSI mide la bajada; la **subida** depende de `potencia_tx` y de la antena.
- La **C3 SuperMini** tiene la antena de chip mal adaptada. De más barato a más prolijo:
  1. **Alambre de 31 mm** (cuarto de onda) en la alimentación de la antena. **Probado:** con `potencia_tx 60` (15 dBm),
     de ~3 m a ~10 m a través de una vidriera, con powerbank. **Con la antena no subir a 19,5 dBm** (con cargador o
     powerbank deja de andar).
  2. **Placa C3 con conector de antena externa** (u.FL/IPEX), por ejemplo la Seeed XIAO ESP32C3.
  3. **Instalación final:** módulos con antena certificada (ESP32-C3-MINI-1 o ESP32-C3-WROOM-02) en placa propia.
- Del lado de la red, la solución son los **nodos de piso** (repetidores). En plan B, una hoja puede colgarse de la raíz
  o de otro concentrador.
