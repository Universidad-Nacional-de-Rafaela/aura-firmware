# Pruebas en placa: actualización por la malla y cortes (2026-10-22)

Banco: las tres placas de siempre (gateway S3 `mac-90e5b1d64a94`, concentrador S3 `mac-dcb4d92a5878`, hoja C3
`mac-907069add964`, aula `aula11`, la hoja midiendo cada 10 s), separadas entre sí, `broker.py` con la AURA falsa en
modo `ok` y `ota.py` en la PC. Firmware: el de esta rama (`feature/mesh-ota`), compilado con ESP-IDF 6.1.

Qué se buscaba ([ADR-008](../docs/adr/ADR-008-actualizacion-por-la-malla-y-tipo-de-firmware.md)):
- que la actualización por la malla ande con las tres placas;
- que **cortar cualquier cosa a mitad de camino** no deje una placa muerta ni pierda datos;
- que el aula **se recupere rápido** de los cortes de siempre.

## Cómo se probó

- **Registro de las tres placas** a la vez, con hora de la PC, en un archivo por placa.
- **Cortes:** se hicieron desde el mismo programa, sin tocar nada a mano:
  - *reinicio:* esptool con `--after hard-reset`;
  - *apagado:* esptool con `--after no-reset`, que deja la placa en el bootloader hasta el próximo reinicio;
  - *broker caído:* se cortó el proceso de `broker.py` y se lo levantó de nuevo.
- **Cortes en medio de una actualización:** un ayudante lanza `ota.py`, espera a que la placa diga
  "Recibido N %" y entonces corta.
- **Pérdidas:** para cada prueba se compararon las mediciones que la hoja guardó, las que AURA confirmó y los
  `ingest_id` distintos que llegaron al broker.
- **Imágenes de cada versión:** se compilaron cambiando solo `PROJECT_VER`. Las que fallan a propósito llevaron
  un cambio temporal en `main.c` que no está en la rama.

## Resultados

### Actualización por la malla

| # | Prueba | Resultado |
|---|---|---|
| 1 | Hoja 1.0.0 → 1.0.1 (`--hw`) | ✅ **31 s** del pedido a `aplicado`: descarga al gateway 8 s, 1,09 MB por la malla en 10 s (2 saltos, por el concentrador), reinicio y vuelta 11 s. *Antes de la espera de estabilidad (abajo); hoy `aplicado` llega ~60 s más tarde.* |
| 2 | Concentrador 1.0.0 → 1.0.1 | ✅ 20 s. La hoja no perdió nada: 3 mediciones esperaron en su cola y salieron al volver el concentrador. |
| 3 | `--todas` con una versión que ya tiene | ✅ `aplicado` con `sin_cambios`, sin transferir nada (el gateway lo sabe por `details.firmware`). |
| 3 | `--todas --aula aula99` | ✅ "No hay placas online de tipo aura-hoja-ejemplo en aula99". |
| 3 | Imagen de hoja al concentrador | ✅ `rechazado` `tipo_distinto` (con `details.firmware` del concentrador), sin transferir. |
| 3 | Imagen de S3 a la hoja C3 con `--forzar-tipo` | ✅ la rechaza la propia hoja al recibir la oferta: `chip_distinto`. |
| 4 | Gateway y hoja pedidos casi juntos (gateway primero) | ✅ (tras la corrección 4) el gateway actualiza primero a la hoja y después a sí mismo. |
| 7 | **Las tres a la vez** | ✅ **69 s**: hoja, concentrador y por último el gateway, una por vez. |
| — | Gateway que se actualiza a sí mismo | ✅ `aplicado` ~55 s después de volver, cuando la versión quedó confirmada. |

### Cortes en medio de una actualización

| # | Corte | Resultado |
|---|---|---|
| 6a | **Hoja reiniciada** al 40 % | ✅ la hoja siguió con su versión vieja. El gateway lo notó por el sondeo, volvió a empezar a los 9 s y terminó en `aplicado` (36 s en total). 5 de 5 datos. |
| 6b | **Concentrador apagado 40 s** al 30 % de la OTA de la hoja | ✅ la hoja dejó de recibir, descartó lo parcial, pasó a plan B. El gateway volvió a ofrecer y terminó en `aplicado`. 9 de 9 datos, cola máxima 5. |
| 6c | **Gateway reiniciado** al 50 % | ✅ `ota.py` lo notó (`uptime_s` menor) y volvió a pedir. El gateway **reusó la imagen ya descargada** (verificó su SHA-256) y la hoja aceptó la oferta nueva descartando la anterior. 38 s, 5 de 5 datos. |
| 6d | **Concentrador reiniciado** al 40 % de **su propia** OTA | ✅ el gateway volvió a empezar por el sondeo; `aplicado` en 37 s. La hoja cambió de padre en 0,2 s. |
| 6e | **Broker caído** ~14 s al 30 % | ✅ la transferencia siguió por la malla. `ota.py` se reconectó y volvió a pedir, y llegó `aplicado`. |

En ningún caso la placa quedó sin firmware: la versión vieja corre hasta que la nueva está completa y verificada, y
lo que la placa tenía pendiente se respalda en la flash antes de reiniciarse.

### Vuelta atrás

| # | Imagen | Resultado |
|---|---|---|
| 8a | Se cuelga a los **3 s** de arrancar | ❌ → corregido. La versión **se confirmó** (habló con el gateway a los 3,2 s) y quedó en un reinicio sin fin; como se colgaba antes de que otra OTA terminara, hubo que **recuperar la hoja por USB**. Ver la corrección 6. |
| 8b | Se cuelga a los **20 s** (con la corrección) | ✅ el bootloader volvió solo a 1.0.0; el gateway contestó `rechazado` `volvio_a_la_version_anterior` (27 s después de `recibido`). |
| 8c | **No puede entrar a la malla** (otro MESH_ID) | ✅ a los 10 min volvió sola a 1.0.0 y el gateway contestó `rechazado` `volvio_a_la_version_anterior`. Antes de volver respaldó su cola: **las 60 mediciones** tomadas sin malla las recuperó la versión anterior y llegaron a AURA con su hora original. |

### Cortes con los datos en curso (sin OTA)

| # | Corte | Resultado |
|---|---|---|
| 5 | **Gateway reiniciado** | ❌ → corregido. El concentrador quedaba en un bucle de `handshake timeout` cada ~10 s hasta pasar a isla a los 60 s, y la hoja estaba **2 min** fuera de alcance. Con la corrección: **el concentrador vuelve en 2 s y el aula está afuera ~4 s**. La hoja ni se entera (cola máxima 1). 9 de 9 datos (corrección 2). |
| 7 | **Hoja reiniciada** (3 veces) | ✅ con la corrección 3 vuelve en **~4 s** (antes, 10 a 27 s). |
| 10 | **Gateway apagado 2 min, hoja reiniciada en isla** | ✅ isla a los 60 s; la hoja se volvió a colgar en 1 s, y reiniciada en isla, en 2,2 s; el concentrador salió de la isla 19 s después de volver el gateway. **Se perdieron 10 mediciones**: las que la hoja tomó en isla antes de reiniciarse, que tenían menos de 5 min y estaban solo en RAM. Es lo esperado con la cola híbrida (ver "Lo que queda"). |
| 11 | Gateway reiniciado (con la corrección 7) | ✅ sin `offline` falso de la hoja; concentrador en 2 s; 10 de 10 datos. |

### Prueba larga, sin intervención

Del **03:51 al 10:02** (6 h 11 min), las tres placas solas, sin tocar nada:
- 0 reinicios, 0 desconexiones y 0 errores en el serie de las tres;
- **2232 mediciones** tomadas, **2232 confirmadas** por AURA y **2232 `ingest_id` distintos** publicados, sin
  repetidos;
- cola máxima 1 y ningún `offline`.

## Lo que se encontró y cómo se corrigió

1. **El primer evento de una placa tras reiniciarse se perdía.** La `seq` de las tramas arrancaba siempre en 0. Si el
   primer reporte después de un reinicio llevaba la misma `seq` que el último evento de antes, el gateway lo tomaba por
   un reintento: lo confirmaba sin publicarlo. Pasaba siempre, porque el arranque es determinista, y AURA se quedaba
   con el `status` viejo; se vio porque el `aplicado` de la primera OTA nunca llegó. Lo mismo podía pasar con el primer
   comando después de reiniciar el gateway. **Ahora la `seq` arranca al azar** en los tres firmwares. Además, cuando
   vuelve un nodo marcado offline, el gateway ya no publica primero los `details` viejos si lo que llegó es un reporte.
2. **Tras reiniciarse el gateway, el concentrador no podía volver.** La pila WiFi reintentaba el padre de antes, y el
   saludo WPA con el gateway nuevo fallaba una y otra vez: `handshake timeout`, y del lado del gateway `reason 15`.
   Una placa recién arrancada se conecta bien. Se probó primero sacando la hoja del medio, sin efecto, y apagando
   **PMF**, que mejoró otra cosa (abajo). **Ahora el concentrador, al perder el padre, lo elige de nuevo con un
   escaneo** (`esp_mesh_set_self_organized(true, true)`) y vuelve en ~1 s.
   - **PMF (802.11w) apagado en el AP de la malla**, en el gateway y el concentrador. Con PMF el hijo tardaba **~17 s**
     en notar que su padre se había reiniciado (SA query), y se veía `MIC mismatch` en los beacons. Sin PMF, el AP nuevo
     lo desconecta en el acto. Hay que apagarlo en `MESH_EVENT_STARTED`: antes de `esp_mesh_start()` la malla lo vuelve
     a activar. Lo que se cede: PMF protege contra desconexiones falsificadas, y la malla ya usa WPA2 con la clave de
     la malla.
3. **La hoja tardaba 10 a 27 s en volver después de reiniciarse.** Escaneaba los 13 canales, 8 s cada pasada, y a
   veces el concentrador no aparecía en la primera. **Ahora guarda en NVS el canal de su último padre** (se escribe solo
   si cambia) y lo escanea primero, escuchando 1,5 s: vuelve en ~4 s.
4. **El gateway se actualizaba antes que los demás** si su pedido llegaba primero en una tanda. **Ahora su propio pedido
   espera 15 s sin otros en la cola**, y no se atiende mientras su versión actual siga sin confirmar.
5. **El gateway tardaba 30 s en notar que el nodo se había reiniciado en medio de una OTA**, porque solo manda bloques
   cuando el nodo los pide. **Ahora, a los 8 s sin pedidos, le reenvía la última ventana.** Un nodo reiniciado contesta
   `sesion_desconocida` y se vuelve a empezar enseguida.
6. **Una versión que habla y se cuelga a los pocos segundos quedaba confirmada** (prueba 8a). **Ahora queda solo si
   habló con el gateway y además lleva 60 s andando.** Si se reinicia antes, el bootloader vuelve a la anterior. El
   gateway publica `aplicado` recién entonces.
   - El nodo confirma "habló" cuando el gateway lo ve volver con la versión nueva y le manda la trama OTA
     **`CONFIRMADA`**. Antes dependía de un `EVENTO_OK`, que necesita el broker: con el broker caído más de 10 min, una
     versión buena habría vuelto atrás.
   - Antes de volver atrás por tiempo, la placa respalda su cola en la flash, y la versión anterior la recupera.
7. **`offline` falso al reiniciar el gateway.** Los nodos que recuerda en NVS se marcaban `sin_tramas` a los 3
   intervalos, sin el margen de arranque de 60 s que ya tenía la tabla de ruteo; se corregía 1 s después. Ahora usa el
   mismo margen.
8. **`ota.py` terminaba si se cortaba el broker.** Ahora se reconecta y vuelve a pedir lo que no terminó.

## Lo que queda (no se corrigió a propósito o queda para después)

- **Mediciones de menos de 5 min solo en RAM.** Si la hoja se reinicia o se corta la luz **durante un corte de la red**,
  las mediciones de los últimos 5 minutos se pierden: la cola híbrida pasa a la flash lo que lleva 5 min sin confirmar,
  para no escribirla en marcha normal. En la prueba 10 fueron 10 mediciones. Si importa más que el desgaste,
  `COLA_A_FLASH_S` se puede bajar (por ejemplo a 60 s).
- **El LWT de la sesión vieja del gateway llega después de que ya se reconectó**, por eso su `status` queda `offline`
  ~2 s. Pasa con `broker.py` y puede pasar con Mosquitto (toma de la sesión). El gateway vuelve a publicar `online` ~4 s
  después de conectarse, así que se corrige solo.
- **La hoja nota la caída de su concentrador recién a los ~11 a 26 s** (pérdida de beacons). No se pierde nada, porque
  queda en su cola, y el plan B arranca ~35 s después.
- **La primera vez, todas las placas se graban por USB** (tabla de particiones nueva). Una placa con firmware anterior
  contesta `nodo_no_responde`.
- **Imágenes sin firma** (contrato, pendiente 23) y **reparto de a una placa por vez**: con muchas hojas por aula, más
  de 10, conviene medir cuánto tarda un aula entera.
- Las pruebas fueron con **una hoja por aula** y a 2 saltos. Falta repetirlas con varias hojas y un aula lejos del
  gateway.
