# Mesh de interior sobre ESP-WIFI-MESH

Diseño de la mesh de AURA desde el 2026-10-07
([ADR-006](../adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md)). Implementa la
**versión 4.0 del contrato** ([`../CONTRATO_MQTT.md`](../CONTRATO_MQTT.md)).

> ⚠️ Compilado y con tests de host, **sin probar en placa**. Ver [`estado-de-banco.md`](estado-de-banco.md).

## Roles

```
hoja (grupo) ──┐
hoja (grupo) ──┼─ESP-WIFI-MESH─► relevo (cátedra) ─ESP-WIFI-MESH─► raíz (cátedra) ─WiFi─► broker AURA
hoja (grupo) ──┘                                          ▲                           (MQTT hw/…)
hoja (grupo) ─────────────────────ESP-WIFI-MESH───────────┘
```

| Rol | Sketch | `mesh_type` | Qué conoce |
|---|---|---|---|
| Raíz, uno por edificio | [`infraestructura/nodo_raiz/`](../../infraestructura/nodo_raiz/) | `MESH_ROOT` fijo | ID y clave de la mesh, WiFi del edificio, broker |
| Relevo, los que hagan falta | [`infraestructura/nodo_relevo/`](../../infraestructura/nodo_relevo/) | `MESH_NODE` | ID y clave de la mesh |
| Hoja, cada dispositivo de un grupo | `dispositivos/<CÓDIGO>/`, sobre [`comun/nodo_mesh.h`](../../comun/nodo_mesh.h) | `MESH_LEAF` | ID y clave de la mesh |

- **Raíz fijo** (`esp_mesh_fix_root(true)` en todos los nodos): ningún nodo intenta ser raíz.
- **Las hojas no reenvían**: un grupo que reflashea o tiene un bug no corta a los demás. La
  cobertura la dan los relevos.
- La mesh trabaja en el **canal del WiFi del edificio** (`MESH_CANAL`; con 0 busca en todos).
- **Sin IP en los nodos.** Solo el raíz tiene IP. Los nodos se mandan tramas con
  `esp_mesh_send()`: la hoja al raíz con destino `NULL` ("el raíz", sea cual sea su MAC), y el
  raíz a una hoja por su MAC. La mesh enruta sola.
- **Identidad**: la MAC de fábrica de la interfaz STA. El raíz la recibe como origen en
  `esp_mesh_recv()` y publica con `hw_id = mac-<MAC en minúsculas>`. No hay tabla de nodos.

## La capa de radio

[`comun/radio_wifi_mesh.h`](../../comun/radio_wifi_mesh.h) es la única parte que depende de la
placa, y es fina a propósito:

| Función | Qué hace |
|---|---|
| `radio_iniciar(rol, &config, cola)` | `WiFi.mode(WIFI_STA)`, DHCP detenido (solo el raíz lo arranca al asociarse al router), `esp_mesh_init`, raíz fijo, tipo de nodo, clave WPA2 de la mesh, `esp_mesh_start`, y una tarea que recibe con `esp_mesh_recv` y encola |
| `radio_conectada()` | hoja y relevo: unidos a un padre; raíz: con IP |
| `radio_reconecto()` | `true` una vez después de cada (re)conexión: la hoja manda su reporte |
| `radio_al_raiz(trama)` | `esp_mesh_send(NULL, …)` con `MESH_TOS_P2P` (reintentos salto a salto) |
| `radio_a(mac, trama)` | `esp_mesh_send(mac, …)` desde el raíz |
| `radio_recibir(&r)` | saca una trama de la cola, con la MAC de origen |

La tarea de recepción solo valida y encola: nunca envía ni bloquea el loop. Lo que descarta lo
cuenta (`version_distinta`, `invalidas`, `desbordes`), y el raíz lo publica en su `status`.

## La trama (v3)

[`comun/protocolo_aura.h`](../../comun/protocolo_aura.h): `version`, `tipo`, `seq`, `largo` y hasta
180 B de payload (cabecera de 5 B). Los tipos y sus valores son los de la v2; `ACK` y `PING`
quedan reservados, sin uso. Una trama v2 (mesh ESP-NOW) se descarta y se cuenta aparte.

| Tipo | Sentido | Payload |
|---|---|---|
| TELEMETRIA | hoja → raíz | `ingest_id` (16 B) · `ts` (uint32 LE, 0 = sin hora) · JSON de `values` |
| CONFIRMACION | raíz → hoja | `ingest_id` · hora del raíz |
| COMANDO | raíz → hoja | el JSON de `hw/<hw_id>/command` tal cual |
| RESULTADO | hoja → raíz | `{command_id, aplicado, motivo, config}` |
| REPORTE | hoja → raíz | `{config, pendientes, descartadas, alimentacion}` |
| ALERTA | hoja → raíz | `{tipo, severity, message, details}` |

## Una medición, de punta a punta

1. La hoja guarda la muestra en su cola en flash con un `ingest_id` nuevo, y la manda al raíz.
2. El raíz publica `hw/mac-<hoja>/data` con `values`, `ingest_id`, `ts` y `adaptador`.
3. AURA (o [`herramientas/ack_falso.py`](../../herramientas/ack_falso.py)) la guarda y publica
   `hw/mac-<hoja>/ack`.
4. Con un resultado válido, el raíz manda la CONFIRMACION, con su hora. La hoja saca la muestra de
   la cola y ajusta su reloj.
5. Sin `ack`, la hoja reintenta la misma muestra a los 15 s, 30 s, 1 min… hasta cada 5 min.

## Qué se conservó de la mesh ESP-NOW

La cola persistente, el `ingest_id` generado en el nodo, la confirmación solo con la respuesta de
AURA, las alertas de sonda una vez por cambio, `set_config` validado y la API de `nodo_mesh.h`
para los grupos (cambia solo `nodo_mesh_iniciar(cb)`). Lo descartado (radio ESP-NOW, ACK de salto,
barrido de canales, nodo de sala, tabla del gateway) está en el tag `mesh-espnow-v2` y en
[`../historico/`](../historico/).
