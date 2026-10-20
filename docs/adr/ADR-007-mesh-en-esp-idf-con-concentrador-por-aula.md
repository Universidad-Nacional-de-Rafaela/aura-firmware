# ADR-007 — La mesh de interior pasa a ESP-IDF nativo, con un concentrador por aula

**Estado:** propuesta, probada en placa de punta a punta · **Fecha:** 2026-10-20 · **Decide:** Matías Wanzenried, a partir del trabajo de Milton Pozzo
**Reemplaza en parte a:** [ADR-006](ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md) (la
implementación en Arduino y los relevos de la cátedra). Sigue vigente lo central de ADR-006:
ESP-WIFI-MESH sin IP en los nodos, con el raíz como único adaptador del contrato.
**Afecta a:** `docs/CONTRATO_MQTT.md` (v4.1), el repositorio público `aura-firmware`
(`Mesh_Red_Completa/` reemplaza a `comun/` e `infraestructura/` para la mesh) y los dispositivos
de interior de los grupos de IC III e IC IV.

## El problema

La mesh de ADR-006 se escribió en Arduino (`comun/radio_wifi_mesh.h`, `comun/nodo_mesh.h`) y se
probó en placa entre el 2026-10-09 y el 2026-10-19 (`aura-firmware/docs/mesh-wifi/estado-de-banco.md`).
En esas pruebas aparecieron problemas que venían de la capa de Arduino y no de la mesh:

- la biblioteca `WiFi.h` tiene su **propia reconexión** y guarda credenciales en la NVS, y se pelea
  con `esp_mesh` por el control de la radio;
- con el router en los canales 12 o 13 la placa se reiniciaba en bucle hasta fijar el país a mano
  (`aura-firmware` #17);
- **no deja elegir el padre**: la mesh cuelga cada hoja del nodo con mejor señal, y una hoja del
  aula 2 puede terminar colgada de un nodo del aula 1;
- cada muestra escrita en flash frenaba el loop unos 2,5 s (bajó a ~40 ms en #18, pero sigue
  escribiendo la flash con cada muestra).

En paralelo, Milton Pozzo armó en ESP-IDF una red de aula completa (raíz, concentrador y hoja) y la
adaptó al contrato v4.0 (`aura-firmware` #21).

## Opciones

**A. Seguir en Arduino (ADR-006) y corregir de a uno.** Los grupos ya conocen el IDE y hay un
dispositivo (`E1-PB-LECA-HFR01`) escrito así. Pero los problemas de `WiFi.h` no se corrigen desde
el sketch, y la elección del padre por aula no se puede hacer.

**B. ESP-IDF nativo, con la misma topología de ADR-006** (raíz y relevos). Saca la capa de
Arduino pero mantiene el problema del padre.

**C. ESP-IDF nativo con un concentrador por aula.** Cada aula tiene un nodo de la cátedra que
anuncia su aula en el beacon (vendor IE); la hoja elige a mano como padre al de su aula y le manda
cada muestra una vez. El concentrador ve los datos del aula, los puede filtrar y los reenvía al raíz
juntos (trama interna LOTE). El aula se guarda en NVS y la asigna AURA con `set_config`.

## La decisión

**C.** La mesh de interior se programa en **ESP-IDF 6.1**, sobre `Mesh_Red_Completa/` de
`aura-firmware`: `Mesh3_Gateway` (raíz), `Mesh3_Intermedio` (concentrador del aula),
`componentes/nodo_mesh` (biblioteca de la hoja) y `Mesh3_Hoja` (ejemplo y base de las hojas de los
grupos). La versión Arduino queda obsoleta para la mesh. LoRaWAN no cambia.

## Qué se gana y qué se pierde

**Se gana:**

- control directo de la radio y de la mesh, sin una capa que reconecte por su cuenta;
- **el padre por aula**: los datos de un aula pasan por su concentrador, que tiene pantalla y mando
  local (botón) sobre las hojas de su aula;
- **las hojas no necesitan el SSID del edificio**: solo el raíz se conecta al router. Cambiar de
  red es reflashear solo el raíz;
- **una sola imagen por tipo de hoja**: ni MAC, ni aula, ni id en el firmware;
- una cola híbrida que no escribe la flash en marcha normal (§5 del contrato).

**Se pierde:**

- **los grupos cambian de herramienta a mitad de cuatrimestre**: ESP-IDF (instalador EIM,
  `idf.py`) en lugar del IDE de Arduino, y C con FreeRTOS en lugar de un `loop()`. El conector en
  Python no cambia;
- `E1-PB-LECA-HFR01` hay que portarlo;
- una placa más por aula (el concentrador) y un tipo de nodo más que mantener;
- **el CI no compila ESP-IDF todavía** (contrato, pendiente 21): hasta que lo haga, un PR que rompe
  la compilación de una hoja pasa en verde;
- un reinicio de la hoja durante un corte pierde sus últimos 5 min de muestras sin confirmar.

## Lo que esta decisión NO resuelve

- Que AURA asigne el aula sola al asignar una placa (contrato, pendiente 20).
- El cifrado de punta a punta y la custodia de la clave de la mesh (contrato, pendientes 8 y 15).
- La antena de la ESP32-C3 SuperMini: con la de fábrica no se conecta de forma confiable; se
  probó con una de alambre de 31 mm y 15 dBm. Para la instalación final, módulos con antena
  certificada.

## Disparador de revisión

- Si la prueba larga (una semana con varias aulas) muestra pérdidas o reinicios que no aparecían en
  Arduino.
- Si más de la mitad de los grupos de interior no logra compilar su hoja en ESP-IDF antes de la
  entrega: volver a evaluar ofrecerles un envoltorio en Arduino sobre la misma biblioteca.
