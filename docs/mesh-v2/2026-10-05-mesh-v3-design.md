# Diseño: la mesh ESP-NOW a la v3.0 del contrato (trama v2)

**Fecha:** 2026-10-05 · **Implementa:** [`CONTRATO_MQTT.md`](../CONTRATO_MQTT.md) v3.0, pendiente 4
de §10 · **Reemplaza a:** [`mesh-v1/`](../mesh-v1/), cuyas lecciones de banco (§9) siguen vigentes

## 1. Por qué

[ADR-003](../adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) trajo de vuelta la mesh
para los dispositivos de interior. La v3.0 del contrato le pide al gateway de la mesh cosas que la
v1 no hacía:

| v1.x | v3.0 |
|---|---|
| telemetría por MQTT (`devices/<id>/data`) | por REST, un evento por request, con criterio de persistencia (§6) |
| `ingest_id` derivado en el gateway de `(MAC, boot_id, seq)` | generado en el **nodo** y guardado con la muestra antes del primer envío (§5) |
| buffer en RAM en el sensor y en la sala | **cola persistente** en el nodo, que conserva la muestra hasta que AURA la confirma |
| `response` = `enviado_a_mesh` | `transmitido`, `recibido`, `aplicado` / `rechazado`, con `command_id` (§3.4) |
| — | `set_config` validado en el nodo, reporte de estado, alertas, offline inferido |
| `pend`, `desc`, `lux_sim` dentro de `values` | diagnóstico en `status.details`; en `values`, solo mediciones |
| MAC, IP y UUID escritos en los `.ino` | en `config_local.h`, que no se versiona (el repo es público) |

## 2. La regla central: confirmación de punta a punta

El ACK de ESP-NOW, el de la sala y el del gateway **no** prueban que la muestra quedó persistida.
Lo prueba solo la respuesta del REST. Por eso hay dos confirmaciones distintas:

- **ACK** (tipo 3): de salto a salto, al que transmitió (`info->src_addr`). Dice "recibí tu trama".
- **CONFIRMACION** (tipo 4): del gateway al nodo de origen, con el `ingest_id`. Dice "AURA la
  persistió". El gateway la manda solo si se cumple
  `HTTP 201 ∧ errors == 0 ∧ inserted + duplicates == 1`.

El nodo saca la muestra de su cola **solo** con la CONFIRMACION. Como el `ingest_id` viaja con la
muestra, un reintento después de cualquier corte o reinicio es un duplicado que el backend
descarta, y el gateway lo confirma igual (`duplicates == 1`).

Esto deja a la sala y al gateway **sin estado durable**. Si se reinician con tramas en RAM, el
nodo las reenvía. Toda la durabilidad está en un solo lugar: la flash del nodo que tomó la
muestra.

```
nodo ──TELEMETRIA──▶ sala ──TELEMETRIA──▶ gateway ──POST──▶ AURA
     ◀──ACK──────────     ◀──ACK───────────        ◀─201────
     ◀──CONFIRMACION─ sala ◀──CONFIRMACION─ gateway   (solo si persistió)
nodo: cola_pop()
```

## 3. Trama v2

La cabecera de 17 B no cambia; `AURA_PROTO_VERSION` pasa a 2. Una trama v1 se rechaza y se
cuenta en `version_distinta` (en el `status` del gateway y en el monitor de la sala): un nodo con
firmware viejo se ve, no desaparece en silencio.

| Tipo | Valor | Sentido | Payload |
|---|---|---|---|
| TELEMETRIA | 1 | sube | `ingest_id` 16 B · `ts` uint32 LE (0 = sin hora) · JSON de `values` (≤160 B) |
| COMANDO | 2 | baja | el JSON del backend, compactado (≤180 B) |
| ACK | 3 | un salto | — |
| CONFIRMACION | 4 | baja | `ingest_id` 16 B · hora del gateway uint32 LE |
| RESULTADO | 5 | sube | `{"command_id","aplicado","motivo","config"}` |
| REPORTE | 6 | sube | `{"config","pendientes","descartadas","alimentacion"}` |
| ALERTA | 7 | sube | `{"tipo","severity","message","details"}` |
| PING | 8 | un salto | — (barrido de canales; antes era una telemetría vacía) |

`mac_destino` es siempre el destino **final**: el gateway para lo que sube, el nodo para lo que
baja. La sala decide sin lógica por caso: si el destino es ella, procesa la trama; si es de
subida, la reenvía al gateway; si es de bajada y el destino es uno de sus `HIJOS`, se la
reenvía a ese hijo.

**La hora.** Los nodos no tienen WiFi ni RTC con batería. La CONFIRMACION lleva la hora del
gateway, que sale de NTP, y el nodo la toma de ahí. Hasta la primera confirmación, sus muestras
van con `ts = 0`, y el gateway omite `ts` (contrato §3.1: no se inventa).

## 4. Piezas

| Archivo | Qué es | Tests de host |
|---|---|---|
| `comun/protocolo_aura.h` | trama v2, armado y lectura de TELEMETRIA y CONFIRMACION | `test_protocolo` |
| `comun/ingest_id.h` | UUID v4 con `esp_random()` | `test_ingest_id` |
| `comun/cola_persistente.h` | anillo en flash con cabecera verificada; el almacenamiento está abstraído | `test_cola` (incluye reinicios, desborde, cabecera corrupta y falla de escritura) |
| `comun/nodo_mesh_logica.h` | alertas una vez por cambio y espera de reintento (15 s ... 5 min) | `test_nodo_logica` |
| `comun/radio_mesh.h` | ESP-NOW compartido por los tres roles: callback que solo encola, ACK, envío con ACK, barrido, verificación de placa | — (placa) |
| `comun/nodo_mesh.h` | **la biblioteca de los nodos de los grupos** | — (placa) |
| `infraestructura/nodo_gateway/gateway_logica.h` | criterio de §6, offline inferido, época → ISO 8601, comandos permitidos | `test_gateway_logica` |

**Cola del nodo.** 1440 muestras (24 h a una por minuto, ~260 KB) en `/cola.bin` de LittleFS, que
se crea una vez con su tamaño final. Cada operación abre y cierra el archivo, porque cerrar es
lo que asegura que quedó escrito. Primero se escribe la ranura y después la cabecera: un corte
entre las dos pierde solo la muestra que se estaba guardando. Si la cola se llena, se pisa la
muestra más vieja y se cuenta en `descartadas`, que va en el reporte.

**Una muestra en vuelo.** El nodo manda la más vieja y espera su confirmación. Si no llega, la
reenvía a los 15 s, 30 s, 1 min… hasta 5 min. Con AURA caída horas, cada nodo pregunta a lo
sumo cada 5 minutos. Al volver, la cola se vacía a la velocidad del POST.

**Eventos** (resultado, reporte, alerta). Van en RAM, con tres intentos. Si se pierden, el
próximo reporte deja todo en orden: el nodo reporta al arrancar, después de un `set_config`, al
volver el enlace y después de un barrido de canales exitoso. Para que una retransmisión de la
sala no se publique dos veces, el gateway descarta un evento cuyo `seq` coincide con el último
del mismo nodo.

## 5. El gateway como adaptador

- **Tabla** `{MAC, device_id, type, MAC del padre, comandos}` en `config_local.h`. Una MAC que no
  está en la tabla suma a `huerfanos` y se descarta. Una trama cuyo `mac_destino` no es el
  gateway suma a `ajenas` (suele ser un nodo con `MAC_GATEWAY` mal puesta).
- **Comandos.** Se rechazan, con el motivo, si el JSON es inválido, si falta `command`, si el
  comando no está en la lista del nodo, si supera 180 B o si la radio no acepta la trama. Si
  sale, el gateway publica `transmitido`. Cuando llega el resultado del nodo, publica
  `recibido` y después `aplicado` (con `config`) o `rechazado`. Si a los 30 s no llegó nada,
  publica `rechazado` con `sin_resultado_del_nodo`: ningún comando queda sin respuesta.
- **Offline inferido.** Con `3 × intervalo_s` sin tramas de un nodo, el gateway publica
  `offline` una sola vez (retain). La próxima trama lo devuelve a `online` con los últimos
  `details`. `intervalo_s` sale de la `config` del reporte. La sala reporta cada 5 min con
  `intervalo_s: 300`, así que también se le detecta la caída.
- **ACK antes que nada.** La cola de la radio se vacía, confirmando cada salto, antes y después
  de cada POST y de cada comando. El timeout del HTTP (2,5 s) es menor que el del ACK de la sala
  (4 s). El cliente HTTP tiene su propio `WiFiClient`: con el de MQTT, cada POST cortaría la
  sesión con el broker.
- **Valores inválidos.** Si el `values` de una muestra no es un objeto JSON, la muestra no va a
  persistir nunca. El gateway la confirma igual y la cuenta en `values_invalidos`, para que no
  trabe la cola del nodo.

## 6. Lo que esto no resuelve

- **El backend todavía rechaza `aplicado`** (pendiente 3 del contrato). El gateway lo publica
  igual, porque es lo que pide la v3.0.
- **Autenticación del REST y ACL del broker** (pendiente 11). El gateway acepta `API_TOKEN`
  opcional en `config_local.h`, para cuando el endpoint lo pida.
- **Cifrado de ESP-NOW** (pendiente 8). `radio_agregar_peer` es el único lugar a tocar.
- **La tabla MAC → device_id va compilada en el gateway.** Agregar un nodo requiere reflashear
  el gateway (ADR-003).
- **Varios gateways por edificio.** Cada uno tiene su tabla; un nodo pertenece a uno solo.

## 7. Cómo se probó

Tests de host con `-Wall -Wextra -Werror` y compilación de todos los sketches para XIAO ESP32S3,
igual que el CI. La prueba en banco con tres placas sigue el procedimiento de §6 y §9.3 de
mesh-v1: camino feliz, backend caído con reinicio del nodo en el medio, `201` con errores,
`set_config` válido, inválido y desconocido, MAC fuera de la tabla, nodo v1, offline inferido,
nodo ocioso que no barre y barrido fallido que restaura el canal. Su resultado se anota en el PR
que introdujo este diseño.
