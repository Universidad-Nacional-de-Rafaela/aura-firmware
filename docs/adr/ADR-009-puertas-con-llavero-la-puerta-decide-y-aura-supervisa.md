# ADR-009 — Puertas con llavero de proximidad: la puerta decide sola y AURA supervisa

**Estado:** aceptado · **Fecha:** 2026-11-05, decisiones abiertas cerradas el 2026-11-13 · **Decide:** Matías Wanzenried, a partir del prototipo del PR #40
**Sigue a:** [ADR-003](ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) (la mesh para interior), [ADR-004](ADR-004-mqtt-punto-comun-de-ingesta-con-ack-de-persistencia.md) (`ack` después de guardar), [ADR-005](ADR-005-identidad-por-placa-y-mapeo-en-aura.md) (identidad por placa), [ADR-007](ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md) (mesh en ESP-IDF) y [ADR-008](ADR-008-actualizacion-por-la-malla-y-tipo-de-firmware.md) (actualización por la malla, imágenes firmadas).
**Afecta a:** [`docs/CONTRATO_MQTT.md`](../CONTRATO_MQTT.md) (v4.5: comandos de llaveros,
`estado` y `abrir`, eventos en `values`, alertas de la puerta), el modelo `dispositivos/_modelos/PUE-RFID/` y sus nueve
instalaciones `E2-P2-*-PUE01`, y la plataforma (registro de ingresos, administración de llaveros y
supervisión de las puertas armadas).

## El problema

A la UNRaf se le propuso controlar el acceso a nueve oficinas del segundo piso del Edificio 2 con
llaveros de proximidad: "quién puede ingresar a cada espacio, abrir las puertas en forma remota y
llevar un registro de los ingresos" (nota al Rector del 2026-10-05). El prototipo del PR #40 anda en
banco, pero está pensado para una puerta sola, con una enroladora emparejada y toda la lógica en la
placa.

Para nueve puertas de oficinas de autoridades hay que decidir:

- qué pasa cuando falla algo: la luz, la red, el firmware, la propia cerradura;
- quién manda sobre la lista de llaveros;
- cómo se detecta que alguien entró sin llavero, si la salida por el picaporte siempre está libre;
- qué se puede hacer desde AURA sin abrir una puerta trasera en la mesh.

## Hechos de partida

- **Cerradura.** El pestillo eléctrico (Luber 500, 8–12 V) reemplaza al cerradero de la cerradura
  existente. Sin corriente queda **trabado**; la corriente lo destraba. **Se quema si queda
  energizado** mucho tiempo.
- **Salida.** Desde adentro, la puerta se abre siempre con el picaporte, y la llave mecánica sigue
  abriendo desde afuera. La salida y la evacuación no dependen del sistema.
- **Sensor.** Un sensor magnético en el marco (MS14) dice si la puerta está abierta o cerrada. No
  distingue a alguien que sale de alguien que fuerza la puerta, ni sabe si el pestillo quedó
  enganchado.
- **Comandos.** Por la mesh, un comando llega en menos de 1 s y ocupa como mucho 180 B
  ([contrato](../CONTRATO_MQTT.md) §3.3).

## Decisiones

**1. Un solo estado de reposo: trabada.** La cerradura se energiza solo en un **pulso**, con tope
de duración (N s, que sale de la hoja de datos de la cerradura; 10 s si no lo dice) y con un límite
de tiempo energizada por minuto: 10 s de tope, 5 s por defecto, como mucho 20 s energizada en
60 s. El tope lo corta un temporizador de software (`esp_timer`) que no depende del bucle
principal, respaldado por el watchdog, que reinicia la placa a los 5 s de un cuelgue; no hay un
corte por hardware. El tope definitivo sale de la hoja de datos de la cerradura (si es de alterna
o de continua, y cuánto aguanta energizada).
**El relé:** el valor por defecto de `CONFIG_PUERTA_RELE_ACTIVO_ALTO` tiene que coincidir con el
módulo que se compre para las nueve puertas, y con ese módulo se verifica que el pin del relé no
lo active durante el arranque (caso F2). Cualquier falla (corte de luz, reinicio, cuelgue, actualización) deja
la puerta trabada.
*Por qué:* es lo que hace la cerradura sin corriente, la salida no depende de ella, y es la única
forma de que un error del firmware no la queme ni la deje abierta.

**2. Sin modos de puerta abierta.** Se descartan el "libre paso" y el modo "emergencia" del
prototipo (pestillo abierto hasta nuevo aviso).
*Por qué:* los dos dejan la cerradura energizada durante horas, y la queman. En una emergencia, los
rescatistas entran con la llave mecánica.

**3. La puerta decide sola; AURA es la fuente de verdad de los llaveros.** Cada placa guarda en NVS
la lista de UIDs de su puerta y decide sin consultar a nadie: funciona aunque no haya red. AURA la
administra con comandos incrementales (`tarjeta_agregar`, `tarjeta_quitar`), porque la lista entera
no entra en 180 B. Entran **32 llaveros** por puerta (`lista_llena` si no hay lugar).
La placa informa en su reporte cuántas tarjetas tiene y un hash de la lista (`config.tarjetas` y
`config.hash`, de solo lectura; el formato del hash está en el contrato). Si no coinciden con lo que
tiene AURA, y no hay comandos de llaveros pendientes para esa puerta, AURA manda una alerta y la
**recarga**: `tarjetas_vaciar` y después un `tarjeta_agregar` por llavero. Mientras recarga, la puerta
no abre con ningún llavero (unos segundos por llavero): se acepta, porque un desfasaje es raro y
la llave mecánica sigue abriendo.
**No hay tarjeta maestra:** una lista que se puede cambiar en la puerta deja de coincidir con la de
AURA. Por lo mismo, la carga por la consola USB (`CONFIG_PUERTA_CARGA_POR_CONSOLA`) es solo para el
banco y va apagada en las puertas instaladas.

**4. Enrolamiento con un lector aparte.** El UID de un llavero nuevo se lee con un lector de banco
(ESP32-C3 + RC522 por USB) y se carga en AURA. El lector y la puerta formatean el UID con el
**mismo** código: hexadecimal en mayúsculas, sin separadores, de 4, 7 o 10 bytes. Como respaldo, la
puerta registra el UID de cada llavero que rechaza.

**5. Sin horarios en esta etapa.** Un llavero habilitado abre su puerta las 24 horas. La hora se
usa para el registro y para el vencimiento de `abrir`:

- la placa la toma de la confirmación del raíz ([contrato](../CONTRATO_MQTT.md) §3.1);
- la conserva en los reinicios por software;
- se corrige con la confirmación de cada evento (si se corrió más de 2 s), y si no tiene hora la
  pide al raíz cada 10 s. El latido no la corrige, porque va como reporte (decisión 7): en una
  puerta sin movimiento durante días, la hora solo deriva lo que derive el reloj de la placa.

Si la placa reinicia sin raíz, los eventos salen sin `ts` y con un contador. Los perfiles horarios
quedan como tema de IC4, junto con un reloj con pila: con horarios, sin hora no se puede decidir
quién entra.

**6. Armado local, con doble pasada.** Dos pasadas de un llavero autorizado en ≤ 3 s, con la puerta
cerrada, la **arman**: la segunda corta el pulso y suena un tono propio. Una pasada desarma y abre.
Armada, abrir la puerta sin desarmar es una **intrusión**:

- se manda una alerta `critical` a AURA;
- suena un aviso local, con tope de 2 min.

Armar y desarmar solo se hace en la puerta.
*Por qué:* resuelve que el sensor no distinga una salida de una intrusión: con la oficina vacía,
nadie sale por el picaporte. No se hace desde AURA por la misma razón que la decisión 8.

**7. Supervisión de las puertas armadas.** Quien fuerza la puerta encuentra la placa del lado de
adentro y puede arrancarla. Por eso:

- **la alerta de intrusión sale antes que cualquier otra cosa**, y además queda en el registro con
  `ingest_id`;
- **una puerta armada manda un latido más seguido**: cada 30 s armada y cada 120 s desarmada. La
  meta es que AURA se entere de una puerta armada sin contacto en **90 s o menos** (el raíz la da
  por caída a los 3 latidos sin noticias); el intervalo se deriva de esa meta;
- **el latido no es una medición: es un reporte de estado** (ver abajo);
- **ante una intrusión, AURA consulta enseguida** con el comando `estado`, que es de solo lectura.
  Si la puerta no responde, o vuelve `nodo_no_alcanzable`, manda una alerta crítica;
- **una puerta armada que pasa a `offline`** (`fuera_de_la_malla` o `sin_tramas`) genera una alerta
  crítica. Si antes hubo una intrusión, las dos alertas se muestran como un solo incidente;
- **si se cae el raíz con puertas armadas**, es otra alerta crítica: puertas sin supervisión.

Por ahora las alertas se ven en el dashboard; quién las recibe y por qué medio se decide después.

**El latido no se guarda.** Solo sirve para que AURA sepa, ahora, que la puerta sigue ahí y en qué
estado está; el latido de hace una hora no le interesa a nadie. Por eso no va como `data` sino como
el **reporte** de la placa (`nodo_mesh_reportar`), que el raíz publica como `status`:

| | Como `data` (el prototipo) | Como reporte → `status` (esta decisión) |
|---|---|---|
| En la placa | entra en la cola de muestras (400) con su `ingest_id` y se reintenta hasta el `ack`; pasados 5 min va a la flash | ocupa un solo lugar en la cola de eventos: un reporte nuevo **reemplaza** al que no salió |
| Sin red | se acumula: armada, la cola se llena en unas 3,3 h y desde ahí se pierden muestras viejas, eventos de acceso e intrusiones incluidos | no se acumula: al volver sale un solo reporte, con el estado de ese momento |
| En AURA | una fila por latido en la serie temporal (2880 por día por puerta armada) | pisa `config_vigente` y `last_seen` del dispositivo: no queda historial |
| En el broker | no retenido | retenido: un AURA que arranca ve enseguida el último estado de cada puerta |

El estado de la puerta viaja dentro de `config`, que es lo único que la placa puede agregar al
reporte: `armada` y `abierta` (0 o 1), además de `intervalo_s`, `tarjetas` y `hash`. Entra en los
180 B de la trama porque `nodo_mesh` saca primero los datos accesorios (`pendientes`, `rssi`…).
El comando `estado` pide un reporte en el momento.

**Casos especiales de una puerta armada:**

- **Actualización o reinicio.** AURA **no manda `ota` a una puerta armada**: se actualiza
  desarmada. Así no hay una alerta falsa de "armada sin contacto" mientras se reinicia.
- **Memoria borrada** (`nvs_flash_erase` al arrancar): la placa arranca con la lista vacía,
  desarmada y sin aula, y lo avisa con una alerta propia; AURA recarga la lista (caso F12).
- **Corte de luz general:** el raíz y las nueve puertas pasan a `offline` a la vez. AURA lo muestra
  como **un solo incidente** de raíz caído, no como nueve alertas. Una UPS para el raíz, el
  concentrador y las placas se evalúa aparte.

**8. Apertura remota deshabilitada al compilar.** `abrir` existe en el contrato, pero el firmware
lo rechaza hasta que se habilita con una opción de compilación (Kconfig), no con `set_config`.
Habilitarla requiere publicar una imagen nueva, que a su vez requiere la firma de la cátedra
(ADR-008). Cuando se habilite, `abrir` llega con vencimiento, la placa rechaza las órdenes vencidas
o que llegan sin hora válida, y además desarma la puerta.
*Por qué:* hoy un comando falso que pase como si viniera del raíz podría abrir cualquiera de las
puertas, y una orden encolada en el broker se podría ejecutar horas después. Se habilita cuando la
etapa 2 de seguridad de la mesh lo resuelva.

**9. Registro de ingresos.** Cada evento (acceso, rechazo, apertura, cierre, armado, desarmado,
intrusión) va como `data` con `ingest_id`, como un **registro de evento** en `values`
(`{"evento": "acceso", "uid": "…"}`), que el contrato v4.5 admite junto a las mediciones; el conector
de la puerta en AURA los separa. Por la mesh viaja el UID, y la persona la asocia AURA.
Sin red, los eventos esperan en la cola de la placa, con límites: la cola es de 400 muestras (sin el
latido, alcanza para días de accesos), un reinicio pierde lo que tenía menos de 5 min sin confirmar,
y las alertas van por otra cola, de 8. Que la cola priorice eventos sobre otras cosas queda como
tema de IC4.
**El registro es un dato personal** (Ley 25.326): un UID asociado a una persona, con fecha y hora.
Se le consulta a la universidad como tema de datos personales: cuánto tiempo se guarda y qué rol de
AURA lo ve. Hasta tener la respuesta, la propuesta de 1 año (lo ven el titular de la oficina y
administración) **no está decidida**. Las debilidades conocidas no se publican en el repositorio
público: van al documento interno.

**10. Un modelo, nueve instalaciones.** El firmware es una hoja de la mesh en ESP-IDF, en
`dispositivos/_modelos/PUE-RFID/`, con tipo `aura-puerta-rfid` (ADR-008). Cada puerta tiene solo su
ficha. Lo que cambia según la puerta va por `set_config`: la duración del pulso (`pulso_ms`, de
500 ms al tope) y el aviso de puerta abierta (`aviso_s`, 15 s por defecto; `0` lo desactiva, para
una oficina que se deja abierta).
**El tipo de sensor no:** se fija al compilar (Kconfig). Las puertas instaladas son siempre `reed`;
`boton` es solo para el banco. Si se pudiera cambiar por `set_config`, un comando falso haría que una
puerta armada y abierta figure cerrada, que es el mismo riesgo por el que se deshabilitó `abrir`.
Por lo mismo, **la puerta no acepta `set_config` con `origen: local`** (el botón del concentrador):
hace falta una opción en `nodo_mesh`, que es de la cátedra.
**Otros valores del prototipo que quedan:** ventana de la doble pasada 3 s; la misma tarjeta apoyada
cuenta una vez cada 700 ms; 5 rechazos en 60 s frenan el lector 30 s; el lector caído reinicia la
placa a los 5 min. **La alerta de sensor sospechoso** (marca "abierta" por 4 h) se apaga hasta que haya
horarios: en una oficina abierta todo el día da una alerta falsa por día.

## Qué se gana y qué se pierde

**Se gana:**

- ninguna falla deja una puerta abierta ni quema una cerradura;
- las puertas funcionan sin red;
- AURA sabe qué llaveros tiene cada puerta y se entera si alguien los cambió;
- una intrusión deja rastro aunque destruyan la placa.

**Se pierde:**

- no hay horarios ni libre paso;
- la apertura remota que se le prometió al Rector queda para después;
- armar la puerta con alguien adentro da una falsa alarma;
- un corte de luz deja la puerta sin registro: se entra con la llave y no queda constancia.

## Lo que esta decisión NO resuelve

- **Las debilidades conocidas del hardware y de la mesh.** Su detalle está en un documento interno,
  fuera de este repositorio.
- **Quién recibe las alertas de intrusión.** Por ahora, solo el dashboard.
- **La topología de E2-P2**: dónde van el raíz y el concentrador.
- **Dónde va la placa en cada puerta**: cuanto más difícil de alcanzar, más tiempo tiene para avisar.
- **La retención del registro y quién lo ve**: consulta a la universidad (decisión 9).
- **Una UPS** para el raíz, el concentrador y las placas.

## Disparador de revisión

- Cuando esté la etapa 2 de seguridad de la mesh: revisar la decisión 8.
- Cuando IC4 tome los horarios: revisar la decisión 5.
- Si la mesh da falsas alarmas de puertas armadas sin contacto: revisar la decisión 7.
- Si cambia la cerradura (otro modelo, que admita trabajo continuo): revisar las decisiones 1 y 2.
- Cuando responda la universidad sobre el registro: revisar la decisión 9.

---

## Anexo A — Casos de uso

Fuentes: **Rector** (nota al Rector y presupuesto del 2026-10-05), **PR** (prototipo del PR #40),
**Rev** (revisión del PR #40 del 2026-11-04), **Discusión** (las decisiones de este ADR),
**Contrato** ([`CONTRATO_MQTT.md`](../CONTRATO_MQTT.md)). "Derivado de" marca una consecuencia
técnica de esa fuente.

### Acceso

| # | Caso | Comportamiento | Fuente |
|---|---|---|---|
| U1 | Llavero autorizado | Pulso (como mucho N s), pitido de OK y registro. Al abrirse la puerta se corta el pulso; si nadie abre, traba al vencer | PR, Rector |
| U2 | Llavero desconocido | Pitido de rechazo y registro del rechazo con el UID | PR; el UID, decisión 4 |
| U3 | Llavero apoyado sobre el lector | Una sola lectura por cada vez que se apoya | derivado de U1 y de la decisión 1 |
| U4 | Muchos intentos rechazados | Frena las lecturas y manda una alerta a AURA | Rev |
| U5 | Salida por el picaporte | Desarmada: se registra como apertura sin acceso. Armada: es una intrusión (U10) | derivado del presupuesto (salida mecánica) |
| U6 | Puerta abierta demasiado tiempo | Aviso sonoro local, alerta a AURA y, al cerrarse, otra alerta `info`. Con `aviso_s: 0`, nada | PR, decisión 10 |
| U7 | Armar | Dos pasadas en ≤ 3 s con la puerta cerrada: corta el pulso, traba y suena el tono de armado. Se guarda en NVS y se registra con el UID | Discusión (sale del PR) |
| U8 | Armar con la puerta abierta | Se rechaza con un pitido de error | derivado de U7 |
| U9 | Desarmar | Una pasada autorizada desarma y destraba. Si se armó sin querer, la tercera pasada lo corrige | Discusión |
| U10 | Intrusión | Armada, se abre sin desarmar: la alerta `critical` sale primero, aviso local con tope de 2 min y alerta `info` al cerrarse | Discusión |
| U11 | Arranque armada y con la puerta abierta | Alerta propia, sin sonido: la placa no sabe qué pasó mientras estuvo apagada | derivado de U7 y F1 |

### Administración desde AURA

| # | Caso | Comportamiento | Fuente |
|---|---|---|---|
| A1 | Alta de un llavero | El UID sale del lector de enrolamiento; se asigna en AURA y se manda `tarjeta_agregar` a sus puertas | Rector, decisión 4 |
| A2 | Baja (llavero perdido o persona que se va) | `tarjeta_quitar` a todas sus puertas; AURA reintenta hasta que cada una lo confirma y muestra cuáles faltan | Rector |
| A3 | Lista desfasada | Por la cantidad y el hash del `status`, sin comandos de llaveros pendientes: alerta y recarga con `tarjetas_vaciar` | decisión 3 |
| A4 | Registro de ingresos | `data` con `ingest_id`; por la mesh viaja el UID | Rector, Contrato, decisión 9 |
| A5 | Configuración | `set_config` con rangos validados: duración del pulso (dentro del tope) y aviso de puerta abierta. El tipo de sensor se fija al compilar. No acepta `origen: local` | Rev, decisión 10 |
| A6 | Apertura remota | Deshabilitada al compilar. Cuando se habilite: con vencimiento, y desarma | Rector, decisión 8 |
| A7 | Consulta de estado | `estado`, de solo lectura: la puerta manda su reporte; AURA la manda sola ante una intrusión | decisión 7 |
| A8 | Puerta armada sin contacto | Alerta crítica; junto con la intrusión, si la hubo | decisión 7 |
| A9 | Raíz caído con puertas armadas | Alerta crítica: puertas sin supervisión. Un corte general es un solo incidente | decisión 7 |
| A10 | Actualización de una puerta | AURA no manda `ota` a una puerta armada | decisión 7 |

### Fallas

| # | Caso | Comportamiento | Fuente |
|---|---|---|---|
| F1 | Corte de luz | Trabada; se entra con la llave; no se registra nada | decisión 1 |
| F2 | Arranque o reinicio | El relé queda en reposo desde el primer instante, en un pin sin glitch al arrancar | Rev, decisión 1 |
| F3 | Firmware colgado | El tope y el watchdog cortan la cerradura | decisión 1 |
| F4 | Pulsos repetidos | Límite de tiempo energizada por minuto | decisión 1 |
| F5 | Relé con el contacto pegado | No se detecta con este hardware: se mitiga con la compra (relé de calidad o fusible térmico) | derivado de la decisión 1 |
| F6 | Sin red o sin raíz | Sigue decidiendo con la lista guardada y encola los eventos | Contrato, Rev |
| F7 | Sin hora | La conserva en reinicios por software; si la pierde, eventos sin `ts` y con contador hasta que el raíz se la vuelve a dar (la pide cada 10 s) | decisión 5 |
| F8 | Lector RC522 caído | Lo reinicia y manda una alerta `sensor` | PR, Contrato |
| F9 | Sensor de puerta trabado o cortado | Cableado para que un corte se lea como "abierta" (sensor `reed`). La alerta por "abierta" durante horas queda apagada hasta que haya horarios | derivado de U6 y U10, decisión 10 |
| F10 | Actualización por la malla | Trabada mientras se reinicia; si la versión nueva falla, vuelve sola a la anterior | Contrato |
| F11 | Placa nueva | Llega a la cuarentena con la lista vacía: no abre con ningún llavero | Contrato |
| F12 | Memoria borrada | Arranca vacía, desarmada y sin aula; manda una alerta propia y AURA recarga la lista | decisión 7 |

### Qué cambia respecto del prototipo del PR #40

- **Se saca:** el modo emergencia, la tarjeta maestra, la enroladora por ESP-NOW y la sirena sin tope.
- **Se invierte:** el relé en reposo deja la cerradura **trabada**.
- **Se conserva:** los pitidos, el aviso de puerta abierta, el armado con doble pasada y la detección
  del lector caído.
- **Se agrega:** el tope y el límite por minuto, la lista administrada desde AURA con su hash, el
  registro con `ingest_id`, el latido (como reporte, no como medición) y la supervisión de las
  puertas armadas.
