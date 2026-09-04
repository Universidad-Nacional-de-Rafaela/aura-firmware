# Diseño: firmware de mesh ESP-NOW para AURA

- **Fecha:** 2026-08-28
- **Estado:** propuesta, pendiente de aprobación
- **Rama:** `feature/firmware-mesh`
- **Contexto:** IC3 — primera capa de firmware del proyecto AURA

## 1. Problema

AURA no tiene capa de firmware. `docs/Arquitectura_IoT_AURA.md` la lista como pendiente
(`- [ ] Desarrollo de firmware para ESP32`) y el informe de avance a AsaCTeI la reporta como
"pendiente de cierre". Los dispositivos existen en el modelo de datos y en el backend, pero
nada los alimenta.

El caso concreto a resolver: un sensor de luz dentro de un aula no llega por WiFi al
gateway del piso. Necesita apoyarse en un nodo intermedio que ya está en la sala.

## 2. Alcance

Tres nodos ESP32 en cadena fija de dos saltos, con tráfico en ambos sentidos:

```
[NODO-SENSOR]          [NODO-SALA]                [NODO-GATEWAY]
 BH1750 (luz)      pantalla + actuadores       pasillo, único con WiFi
      │                    │                            │
      │───── ESP-NOW ─────►│───────── ESP-NOW ─────────►│
      │◄─────comandos──────│◄────────comandos───────────│
                                                        │ MQTT + HTTP
                                                        ▼
                                                  backend AURA
```

**Fuera de alcance:** rutas dinámicas, más de un salto intermedio, cifrado ESP-NOW,
OTA, y cualquier nodo adicional. Se diseñan de modo que agregarlos no obligue a
reescribir el protocolo, pero no se implementan.

## 3. Decisiones de diseño

### 3.1 Ruta fija, padre configurado

Cada nodo tiene la MAC de su padre en configuración. El sensor solo habla con la sala;
la sala solo con el gateway. No hay descubrimiento ni elección de ruta.

**Por qué:** es depurable por alumnos, el reenvío se lee explícito en el código, y para
el piloto del Edificio 1 alcanza. **Costo aceptado:** si cae el nodo de sala, la
habitación queda incomunicada; se mitiga con buffer, no con rutas alternativas.

### 3.2 Solo el gateway conoce AURA

Los nodos sensor y sala se identifican por **MAC** y desconocen UUID, tenant, MQTT y HTTP.
El gateway mantiene la tabla `MAC → {device_id (UUID), tenant_id}` **hardcodeada en
firmware** y traduce en ambos sentidos.

**Por qué:** `TelemetryEventCreate` exige `device_id` como UUID de 16 bytes; mandar eso
por ESP-NOW en cada trama es desperdicio, y obligaría a reflashear los nodos hoja ante
cualquier cambio del backend. La frontera deja el firmware de sensor y sala chico y
testeable aislado.

**Costo aceptado:** dar de alta un nodo nuevo obliga a reflashear el gateway. Con 3 nodos
es irrelevante; a partir de ~10 conviene migrar a que el gateway lea la tabla desde la API
usando el campo `mac_address` que el modelo `Device` ya tiene.

### 3.3 Dos vías de subida, no una

El backend expone dos caminos con contratos distintos, y **no** son equivalentes:

| Vía | Contrato | Idempotente |
|---|---|---|
| MQTT `devices/{device_id}/data` | `{"values": {...}, "unit": {...}, "quality": N}`; solo `values` es obligatorio (`device_integration.py:_validate_data_payload`) | **No** |
| REST `POST /api/v1/telemetry/ingest` | `TelemetryEventCreate`: `tenant_id`, `device_id`, `type`, `payload`, `ingest_id` | **Sí**, por `ingest_id` con índice único |

**Decisión:** el gateway usa **las dos**, cada una para lo que sirve.

- **MQTT** para estado (`devices/{id}/status` con LWT), comandos (`devices/{id}/command`)
  y respuestas (`devices/{id}/response`). El downlink obliga a una suscripción de todos modos.
- **REST `/ingest`** para la telemetría acumulada, en lotes, con `ingest_id`.

**Por qué:** el buffer y el reintento son el corazón de una cadena de dos saltos, y
reintentar por MQTT duplicaría filas. La idempotencia del endpoint REST es lo único que
hace seguro el reintento.

**Costo aceptado:** el firmware del gateway carga un cliente HTTP además del MQTT.

### 3.4 Trama ESP-NOW única para ambos sentidos

```c
typedef struct __attribute__((packed)) {
  uint8_t  version;        // 1
  uint8_t  tipo;           // TELEMETRIA | COMANDO | ACK
  uint8_t  mac_origen[6];  // quién generó el mensaje
  uint8_t  mac_destino[6]; // destinatario FINAL, no el próximo salto
  uint16_t seq;            // secuencia por nodo origen
  uint8_t  largo;          // bytes útiles en payload
  uint8_t  payload[180];
} TramaAura;
```

`mac_destino` es lo que permite a la sala decidir sin lógica por caso: si el destino es
ella, lo procesa; si no, lo reenvía a su otro vecino. `seq` sirve para el ACK salto a salto
y, en el gateway, para derivar un `ingest_id` determinístico.

### 3.5 Buffer y reintento

Sensor y sala mantienen un buffer circular en RAM (~30 muestras). ACK salto a salto;
sin ACK, reintento con backoff. Si el buffer se llena, se pisa lo más viejo y se cuenta el
descarte como métrica publicada en el `status` del nodo.

El `ingest_id` lo deriva el gateway de `(mac_origen, boot_id, seq)`, de forma determinística:
un mismo dato reintentado produce el mismo UUID y el índice único lo descarta.

## 4. Restricción crítica: el canal

**ESP-NOW transmite en el canal en que está la radio.** El gateway necesita estar asociado
al AP para MQTT y HTTP, lo que lo clava en el canal del AP. En consecuencia:

- Los tres nodos deben operar en el canal del AP.
- Si el AP cambia de canal (roaming, DFS, reconfiguración de red), **la mesh se cae en
  silencio**: el gateway sigue online contra AURA y los nodos hoja quedan mudos sin error visible.

**Mitigación:** el gateway publica su canal actual en `devices/{id}/status`, y los nodos
hoja consideran perdido el enlace tras N segundos sin ACK y hacen un barrido de canales
buscando a su padre. Esto es requisito, no mejora opcional.

**Validación previa:** el spike de descubrimiento por broadcast (fuera de este repo, en
`clases/4_Ing_en_comp_3/`) existe para confirmar en el aula que tres placas se ven en un
mismo canal antes de construir sobre esta premisa.

## 5. Estructura

```
firmware/
├── comun/
│   ├── protocolo_aura.h      // TramaAura, tipos, constantes de versión
│   └── buffer_circular.h     // buffer + reintento, compartido por sensor y sala
├── nodo_sensor/
│   └── nodo_sensor.ino       // BH1750 → padre
├── nodo_sala/
│   └── nodo_sala.ino         // pantalla, actuadores, reenvío bidireccional
└── nodo_gateway/
    └── nodo_gateway.ino      // ESP-NOW ↔ MQTT/REST, tabla MAC→UUID
```

## 6. Pruebas

Sin hardware no hay CI posible, así que las pruebas son de banco y por capas:

1. **Protocolo aislado:** compilar `protocolo_aura.h` y `buffer_circular.h` en el host
   (g++) con un test que verifique tamaño de trama, serialización y política de descarte
   del buffer. Es la única parte automatizable y cubre la lógica más propensa a error.
2. **Dos nodos:** sensor + sala en banco, verificar que el dato llega y se muestra.
3. **Cadena completa:** los tres, verificar la fila en `ts_telemetry`.
4. **Reintento:** apagar el gateway 60 s con el sensor emitiendo; al volver, confirmar que
   no hay filas duplicadas (esto valida el `ingest_id`).
5. **Downlink:** publicar en `devices/{uuid}/command` y verificar que el actuador responde
   y que aparece el `response`.

## 7. Riesgos

| Riesgo | Impacto | Mitigación |
|---|---|---|
| El AP cambia de canal | Mesh muda sin error | Barrido de canales en nodos hoja (§4) |
| Doc y código difieren en tópicos | Firmware que nadie escucha | El firmware sigue al **código**; actualizar `Arquitectura_IoT_AURA.md` aparte |
| `main` y `develop` desactualizadas | PR contra rama vacía | Definir destino del PR antes de abrirlo |
| Buffer en RAM se pierde al reiniciar | Hueco de datos | Aceptado en esta etapa; NVS queda para después |

## 8. Divergencias con la documentación existente

Estas quedan pendientes de resolver en los docs, no en el firmware:

1. `Arquitectura_IoT_AURA.md` §1.1 define el gateway como Raspberry Pi 4; acá es un ESP32.
   Hay precedente en `MASTER_PLAN_AURA.md:78` ("ESP32: Gateway secundario para áreas remotas").
2. El doc define tópicos `aura/devices/{id}/sensors/{tipo}/data`; el código usa
   `devices/{id}/data`. **El firmware sigue al código.**
3. ESP-NOW y mesh no figuran en ninguna documentación de AURA. Este documento es su
   primera definición.

## 9. Lecciones del banco

Esta sección se agregó **después** de armar la cadena de tres nodos en hardware,
el 2026-08-28. Documenta los defectos que aparecieron ahí y las reglas que salen
de ellos, porque ninguno era evidente leyendo el diseño.

**Contexto que importa:** la lógica testeable en host (trama, buffer, `ingest_id`)
pasó 1.084 verificaciones con `-Wall -Wextra -Werror` antes de tocar una placa.
Los cuatro defectos que siguen sobrevivieron a eso, a la revisión del plan y a la
lectura del spec. Los encontró enchufar tres placas y leer los logs.

### 9.1 Los defectos

**A. ACK enviado desde el callback de recepción.** El gateway contestaba el ACK
dentro de `on_recv`, que corre en la tarea de WiFi. En el sensor y la sala eso
funcionaba, porque no hacen otra cosa; en el gateway, con el stack TCP ocupado
publicando MQTT, el `esp_now_send()` del ACK se perdía. La sala nunca sacaba la
trama de su cola y la retransmitía indefinidamente, y el gateway republicaba cada
retransmisión: el mismo dato salía al broker una y otra vez.

**B. El perro guardián ladrando sin motivo.** El barrido de canales se disparaba
por `millis() - ultimo_ack_ok > SIN_ACK_MAX`, pero `ultimo_ack_ok` solo se refresca
al enviar con éxito. Un nodo **ocioso** nunca lo refrescaba, llegaba al minuto y
barría canales con el enlace perfecto — rompiéndolo.

**C. Barrido fallido que abandona la radio.** Si el barrido recorría los 13 canales
sin encontrar al padre, salía sin restaurar nada y **dejaba la radio en el canal 13**.
El nodo quedaba varado hablándole a nadie hasta el siguiente intento.

B y C se retroalimentaban: la sala barría sin motivo (B), el sensor no la encontraba
durante ese hueco, su propio barrido fallaba y quedaba varado en el 13 (C). El
resultado era un enlace que había funcionado y dejó de hacerlo sin que cambiara nada
del entorno.

**D. Sketch flasheado en la placa equivocada.** Pasó dos veces con tres placas
idénticas. El síntoma —no llega nada, valores en su estado inicial— es
indistinguible de una falla de radio, y manda a depurar en el lugar equivocado.

**E. El ACK dirigido al origen en vez de al salto anterior.** La sala reenvia la
trama del sensor **sin modificarla**, asi que `mac_origen` sigue siendo el sensor
aunque quien transmitio sea la sala. El gateway confirmaba a `mac_origen`, es decir
al sensor —que ya estaba confirmado por la sala— y la sala quedaba esperando un ACK
que nunca iba a llegar, retransmitiendo la misma trama indefinidamente.

El defecto es de diseño, no de codificacion: **la trama no tiene ningun campo que
identifique al salto anterior**, solo origen y destino final (§3.4). Esa informacion
si esta disponible en `info->src_addr` del callback de ESP-NOW, que es la MAC de
quien transmitio realmente. Se estaba descartando con `(void)info`.

Vale la pena notar que el sintoma tardo en aparecer: con un solo salto (sensor a
sala) origen y salto anterior coinciden, y todo funciona. El defecto solo se
manifiesta al agregar el segundo salto, que es cuando dejan de ser lo mismo.

### 9.2 Reglas que salen de esto

1. **El callback de recepción solo valida y encola.** Nunca envía, nunca bloquea,
   nunca toca TCP. Todo lo demás va al `loop`. A y el reenvío de comandos de la sala
   son el mismo defecto en dos lugares distintos.
2. **Un mecanismo de recuperación solo actúa ante evidencia positiva de falla.**
   "Hace rato que no confirmo nada" no es evidencia de nada si no había nada para
   confirmar. La condición correcta es *hay datos sin confirmar*, no *pasó el tiempo*.
3. **Toda rutina que cambie estado global de la radio lo restaura en todos sus
   caminos de salida**, incluido el de fracaso.
4. **Las confirmaciones van al salto anterior (`info->src_addr`), nunca al origen
   del dato.** En una cadena de un solo salto los dos coinciden y el error es
   invisible; a partir del segundo salto, confirmar al origen deja al intermediario
   retransmitiendo para siempre.
4. **Con placas idénticas, cada sketch declara en qué placa debe correr** y lo grita
   al arrancar si no coincide (`MAC_ESPERADA` / `verificar_placa()`). Convierte media
   hora de depuración en dos segundos de lectura.

### 9.3 Implicancia para las pruebas

Los tres defectos de código están en el **camino de manejo de errores**, no en el
camino feliz: reintento, confirmación y recuperación de enlace. Es código que casi
no se ejecuta durante el desarrollo normal y que, cuando se ejecuta, lo hace en el
peor momento.

De ahí que la prueba 4 de la §6 —apagar el gateway y confirmar que no hay
duplicados— sea la más valiosa del conjunto: es la única que ejercita ese camino a
propósito. Conviene agregarle dos más en la misma línea: dejar un nodo ocioso más de
`SIN_ACK_MAX` y verificar que **no** barre canales, y forzar un barrido fallido
(padre apagado) y verificar que la radio vuelve al canal de origen.

### 9.4 Una advertencia sobre el canal

Durante estas pruebas el AP de `UNRaf_Libre` estaba en el canal 1, que resulta ser
el canal por defecto de un ESP32 en modo STA sin asociar. **Los tres nodos se vieron
por casualidad, no por diseño.** Con el AP en el canal 6, el gateway habría quedado
ahí y los nodos hoja en el 1, sin verse nunca y sin ningún error visible. Es
exactamente el modo de falla de la §4, y la razón por la que el barrido de canales
—una vez que funcione bien— no es opcional.
