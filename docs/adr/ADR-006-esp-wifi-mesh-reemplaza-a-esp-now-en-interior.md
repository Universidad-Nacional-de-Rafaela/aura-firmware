# ADR-006 — ESP-WIFI-MESH reemplaza a la mesh propia sobre ESP-NOW en interior

**Estado:** propuesta, **sin probar en placa** · **Fecha:** 2026-10-07 · **Decide:** Matías Wanzenried
**Reemplaza en parte a:** [ADR-003](ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) (la
tecnología de la mesh). El criterio de ADR-003 sigue vigente: interior por mesh, exterior o lejos
por LoRaWAN.
**Afecta a:** `docs/CONTRATO_MQTT.md` (v4.0), el repositorio público `aura-firmware` (`comun/`,
`infraestructura/`, `ejemplos/`) y los dispositivos de interior de los grupos de IC III e IC IV.

## El problema

La mesh de interior de ADR-003 es un **protocolo propio de la cátedra sobre ESP-NOW**: tramas
binarias, ACK de salto, barrido de canales, un nodo de sala con su lista de hijos y un gateway con
una tabla de MAC. Funciona y pasó por placa, pero:

- **la topología es fija y a mano**: dos saltos como máximo, cada nodo con la MAC de su padre y
  del gateway en su configuración, y la sala con la lista de sus hijos. Agregar un piso o mover un
  nodo es reflashear;
- **el reemplazo del gateway obliga a reflashear todos los nodos**, o a custodiar una MAC lógica
  por placa de infraestructura (contrato v3.1, pendientes 14 y 16);
- **ESP-NOW sin cifrar admite 20 peers** por placa, y cifrado, menos;
- los alumnos estudian un protocolo que no existe fuera de la cátedra.

En el Edificio 1 los dispositivos van a estar repartidos en planta baja, primer piso y azotea, y
las losas atenúan mucho los 2,4 GHz: hacen falta relevos igual, y conviene que las rutas se armen
solas.

## Opciones

**A. Seguir con ESP-NOW (ADR-003).** Ya probado en placa y con menos aire ocupado (los nodos no
emiten beacons). Topología fija y mantenimiento a mano.

**B. ESP-WIFI-MESH con IP en cada nodo** (`ip_internal_network` o ESP-Mesh-Lite). Cada nodo es un
cliente MQTT. Cada placa lleva credenciales del broker, más memoria y más superficie de ataque, y
cambia el modelo del contrato (el adaptador desaparece).

**C. ESP-WIFI-MESH sin IP en los nodos, con el raíz como gateway.** Los nodos se mandan paquetes
con `esp_mesh_send()` y solo el raíz tiene IP. El raíz es el adaptador del contrato, igual que el
gateway de ESP-NOW.

**Se elige C.**

## La decisión

1. **Interior por ESP-WIFI-MESH** (la API `esp_mesh` de ESP-IDF, que el core de Arduino trae
   compilada), **sin IP en los nodos**.
2. **Raíz fijo** (`esp_mesh_fix_root`): una placa de la cátedra por edificio, la única que se
   asocia al WiFi del edificio, tiene IP y conoce el broker. Es el adaptador de la mesh.
3. **Relevos solo de la cátedra** (`MESH_NODE`). Los dispositivos de los grupos son **hojas**
   (`MESH_LEAF`): no reenvían tráfico, así que un grupo que reflashea o tiene un bug no corta a
   los demás.
4. **Arduino sobre XIAO ESP32S3**, como hasta ahora. *(Actualizado el 2026-10-09: la placa de la
   mesh es la **ESP32-C3 SuperMini**; la XIAO ESP32S3 queda para LoRaWAN. Se probó en placa.)* La API de `comun/nodo_mesh.h` para los grupos
   se conserva, salvo `nodo_mesh_iniciar(cb)`, que deja de recibir MAC.
5. **La identidad es la MAC de fábrica** que `esp_mesh_recv()` entrega como origen
   (`hw_id = mac-…`, [ADR-005](ADR-005-identidad-por-placa-y-mapeo-en-aura.md)).
6. **La trama interna pasa a la versión 3**: los mismos tipos y límites que la v2, sin las MAC en
   la cabecera.
7. **El código ESP-NOW sale de `main` de `aura-firmware`** y queda en el tag `mesh-espnow-v2`, con
   su documentación en `docs/historico/`.

## Qué se gana y qué se pierde

**Se gana:**
- **ruteo automático entre pisos**: la mesh elige padre y se repara sola si se cae un relevo;
- **reemplazar el raíz no reflashea ningún nodo**: los nodos buscan la mesh por su ID, no por
  una MAC;
- sin límite de 20 peers, y payloads de hasta 1472 B en la radio (la trama sigue en 180 B);
- autenticación de los nodos con la clave de la mesh (WPA2) y el IE de mesh cifrado por defecto;
- una tecnología documentada por Espressif, que los alumnos pueden estudiar.

**Se pierde:**
- **código propio que ya pasó por placa**: la radio, el ACK de salto y el barrido de canales de
  ESP-NOW se descartan. La cola persistente, el `ingest_id`, la confirmación de punta a punta, las
  alertas y `set_config` se conservan;
- **aire**: cada raíz y cada relevo emiten beacons en el **mismo canal que el WiFi del campus**
  (ESP-WIFI-MESH tiene que trabajar en el canal del router). Con pocos relevos es despreciable;
  con decenas se nota;
- **APs ajenos**: los nodos con hijos aparecen como APs. Hay que coordinarlo con Sistemas antes de
  instalar;
- la combinación Arduino + `esp_mesh` está menos documentada que ESP-IDF puro.

## Lo que esta decisión NO resuelve

- **No se probó en placa.** El código compila, pero quedan por verificar en el banco
  (`aura-firmware/docs/mesh-wifi/estado-de-banco.md`): que una hoja se una **sin la clave del
  WiFi del edificio**, que el raíz tome IP y hable MQTT con la mesh andando, la MAC de origen a dos
  saltos, la bajada por MAC y el reenganche después de reiniciar el raíz. Si los dos primeros
  fallan, esta decisión se reabre.
- **El canal:** si el WiFi del edificio cambia de canal solo, la mesh tiene que seguirlo
  (`allow_channel_switch`). Sin verificar.
- **La clave de la mesh** se comparte entre todos los nodos de un edificio y la conocen los
  grupos. Rotarla es reflashear todas las placas.

## Disparador de revisión

Reabrir si pasa alguna de estas cosas:
- una hoja no puede unirse sin la clave del router;
- `WiFi.h` de Arduino y `esp_mesh` no conviven en el raíz;
- los beacons de los relevos degradan el WiFi del campus o Sistemas los bloquea;
- hace falta que los dispositivos de los grupos también sean relevos.
