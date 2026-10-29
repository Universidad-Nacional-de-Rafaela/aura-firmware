# E1-PB-LECA-HFR01 — Monitoreo de heladera y freezer

| | |
|---|---|
| **Ubicación** | Propuesta: Edificio 1, planta baja, LabECA; confirmar instalación. |
| **Responsable** | Grupo de IC IV 2026: @VainstubTomas, @Bernidb, @Kinggrass265, @JuanCruz46. |
| **Estado** | Firmware portado a ESP-IDF; pendiente la validación física con la C3. |
| **Instalado** | Todavía no. |
| **Transporte** | Interior: hoja ESP-WIFI-MESH; no reenvía tráfico de otras placas. |
| **Contrato** | [Copia MQTT v4.3 (propuesta)](../../docs/CONTRATO_MQTT.md), [ADR-007](../../docs/adr/ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md) y [ADR-008](../../docs/adr/ADR-008-actualizacion-por-la-malla-y-tipo-de-firmware.md). |
| **Basado en** | [Mesh3_Hoja](../../Mesh_Red_Completa/Mesh3_Hoja/) y su biblioteca pública `nodo_mesh`. |
| **Firmware** | Tipo `aura-heladera-freezer`, versión `1.0.0`; mismo binario para las placas del mismo uso. |
| **Códigos anteriores** | Ninguno confirmado. |

## Qué hace

Una **ESP32-C3 SuperMini** lee dos sondas DS18B20: una de heladera y otra de freezer.
Cada una tiene su intervalo y sus umbrales. El LED de alarma se enciende cuando
cualquiera sale de su rango, incluso sin conexión. Una pantalla SH1106 muestra
temperaturas y pendientes; el DS3231 con pila permite conservar UTC sin red.

La biblioteca de la cátedra administra la conexión, el concentrador de aula, la
cola híbrida y los ACK de AURA. El dispositivo usa exclusivamente su API pública:
no accede a variables internas ni a `esp_wifi_*` o `esp_mesh_*`.
El conector Python de [conectores/E1-PB-LECA-HFR01](../../conectores/E1-PB-LECA-HFR01/)
valida y guarda mediciones, y genera alertas al cruzar umbrales.

## Organización del código

| Archivo o carpeta | Responsabilidad |
|---|---|
| `CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv` | Proyecto ESP-IDF 6.1, C3, tipo/versión, dos particiones OTA, rollback y cola de 256 KiB. |
| `main/main.c` | Arranque y tarea de medición; intervalos independientes y API de AURA. |
| `main/hardware.h` | Propuesta de GPIO y opciones de pantalla, RTC y entrada de alimentación. |
| `main/config_local.ejemplo.h` | Plantilla del ID y la clave privados de la mesh. |
| `main/logica.c/.h` | Rangos, alarma, CRC, registro de corte y calendario UTC. |
| `main/config_parser.c`, `main/configuracion.c/.h` | Parche JSON, validación completa, aplicación y reporte de configuración. |
| `main/almacenamiento.c/.h` | NVS de configuración y registro protegido del primer corte. |
| `main/sondas.c/.h` | Dos buses 1-Wire, detección y lectura de DS18B20 con el controlador de Espressif. |
| `main/bus_i2c.c/.h` | Bus I2C compartido por OLED y RTC. |
| `main/reloj.c/.h` | DS3231, validación de la hora y reloj de sistema. |
| `main/pantalla.c/.h` | SH1106 128×64 y LEDs externos de umbral y desconexión. |
| `main/energia.c/.h` | Antirrebote de alimentación y captura local de la primera lectura del corte. |
| `main/idf_component.yml`, `dependencies.lock` | Dependencias declaradas y versiones resueltas por ESP-IDF. |
| `tests/` | Pruebas de lógica y configuración en computadora; no se cargan en la placa. |

La versión anterior de Arduino queda en el historial de Git. Este proyecto se
compila directamente desde esta carpeta con los componentes de
`Mesh_Red_Completa/`, sin `.ino` ni `comun/nodo_mesh.h`. La carpeta
`autocontenido/`, su generador y su verificación en CI fueron retirados del
repositorio: no se generan ni se versionan copias del firmware.

## Hardware

Los siguientes GPIO son una **propuesta nueva para la C3**, no una equivalencia
de los D0…D5 de la XIAO. Confirmar cableado y niveles antes de energizar.

| Componente | Conexión inicial en `main/hardware.h` |
|---|---|
| ESP32-C3 SuperMini | Flash de 4 MiB; placa provista por el proyecto. |
| DS18B20 heladera | GPIO3, bus independiente con pull-up externo de 4,7 kΩ a 3,3 V. |
| DS18B20 freezer | GPIO4, otro bus con su pull-up de 4,7 kΩ a 3,3 V. |
| LED umbral, externo con resistencia | GPIO7, activo alto. |
| LED desconexión, externo con resistencia | GPIO10, activo alto. |
| OLED SH1106 128×64 | SDA GPIO5, SCL GPIO6; dirección 0x3c. |
| RTC DS3231 con pila | Mismo I2C, dirección 0x68; hora guardada en UTC. |
| Detección de alimentación | Deshabilitada (`HFR_PIN_ALIMENTACION = -1`) hasta confirmar el circuito. |

Evitar GPIO18/19 (USB) y GPIO2/8/9 (arranque) en este cableado. Todos los GPIO
trabajan a 3,3 V; revisar también los pull-up de los módulos I2C. Las sondas se
usan con alimentación externa, sin modo parásito. La resolución es de 9 bits
(0,5 °C); cada conversión ocupa unos 100 ms de la tarea de medición.

El LED de desconexión indica **asociación a la mesh**, según la API pública.
No distingue por sí solo una caída del broker o de AURA: se observa también la
cola de pendientes. Tras 15 s sin mesh, o al detectar batería, se apaga la OLED
para ahorrar energía; medir y evaluar umbrales continúa. No se usa deep sleep,
porque la hoja debe permanecer en la mesh.

## Lo que envía a AURA

La identidad es `hw_id = mac-<MAC de fábrica>`. El raíz publica en
`hw/<hw_id>/data`; el firmware no contiene UUID, aula, MAC del padre ni datos
del broker. AURA asigna el dispositivo y el aula después del alta. Todas las
placas de este tipo llevan el mismo binario y parámetros de mesh.

| Campo de `values` | Unidad | Rango válido | Intervalo inicial |
|---|---|---|---|
| `temp_heladera_c` | °C | -55…125, excepto el centinela 85 | 60 s |
| `temp_freezer_c` | °C | -55…125, excepto el centinela 85 | 300 s |

Se omiten lecturas fallidas, no finitas, -127 y 85; no se envían valores ficticios,
`null` ni diagnósticos entre las temperaturas. Cuando vencen ambos intervalos,
las dos lecturas válidas se incluyen en una muestra. En otros ciclos solo se
envía la sonda que corresponde medir. El JSON queda por debajo de los 160 bytes
permitidos para `values`.

Las muestras se encolan inmediatamente tras medir; `nodo_mesh` asigna la hora
y el `ingest_id`, y las libera con la confirmación de AURA. Si hay un RTC válido,
se carga UTC al arrancar; la mesh ajusta el reloj. Sin RTC ni hora de la mesh,
no se inventa una fecha: se aplica el mecanismo de uptime de la biblioteca.

Falla y recuperación de sonda usan `nodo_mesh_sonda()` (alerta `sensor` por
transición). Los cambios de alimentación usan `energia` con
`details.alimentacion = "red" | "bateria"`. Las alertas de temperatura las
genera el conector; el LED de umbral se resuelve localmente.

## Configuración remota

`set_config` valida el parche completo antes de modificar nada. Solo informa
éxito si la escritura y el commit en NVS terminan bien. Un rechazo conserva
los ajustes anteriores. Arranca con valores iniciales si no hay configuración
válida guardada. No borra toda la NVS automáticamente si esta falla.

| Parámetro | Significado y rango | Inicial |
|---|---|---|
| `intervalo_s` | Heladera: entero 5…86400 s. También lo usa el raíz para inferir desconexión. | 60 |
| `f_s` | Freezer: entero 5…86400 s. | 300 |
| `hlo`, `hhi` | Mínimo y máximo de heladera, °C, -55…125, mínimo < máximo. | 2, 6 |
| `flo`, `fhi` | Mínimo y máximo de freezer, mismas reglas. | -25, -15 |
| `aula` | Asignada y guardada por la biblioteca de AURA; el dispositivo no la fija. | Sin asignar |

Los umbrales aceptan hasta dos decimales, aunque la resolución de la sonda es
0,5 °C. Se rechazan tipos incorrectos, nombres desconocidos, claves duplicadas
del dispositivo y rangos invertidos. Los campos ausentes conservan su valor.

```json
{"command":"set_config","params":{"intervalo_s":120,"f_s":600},"command_id":"hfr-1"}
```

Usar parches y `command_id` cortos: el comando completo tiene un límite de
180 bytes. La configuración vigente se agrega mediante `describir_config`.
El parámetro anterior `r_s` se rechaza con
`reintentos_gestionados_por_biblioteca`: la API actual no permite cambiar esos
tiempos. Tampoco se configuran DR, ADR ni uplinks confirmados de LoRaWAN.

## Cola, cortes y límites pendientes de la API

La **única cola de mediciones** es la de la cátedra: hasta 400 registros en RAM
(puede reducirse si no hay memoria), respaldo en flash cuando una muestra lleva
5 minutos sin confirmación, recuperación tras reinicio y descarte de la más vieja
al llenarse. En operación normal las mediciones no escriben flash. Un reinicio
puede perder los últimos 5 minutos que todavía estaban solo en RAM; no equivale
a persistencia inmediata de todas las lecturas.

Para la primera captura de un corte existe además un registro pequeño, con
UTC, máscara de validez y dos temperaturas, protegido por CRC en NVS. Se guarda
la hora incluso si ambas sondas faltan, y se completa con la primera conversión.
Solo se activa al configurar y conectar la entrada de alimentación. Si no hay
hora válida, conserva `utc = 0` y no afirma conocer la hora exacta.

La API compartida todavía no ofrece todas las capacidades del dispositivo:

- **Primera captura:** la cola oficial no permite fijar un registro contra
  overflow ni recibir un ACK correlacionado para retirarlo. El corte local no
  se sobrescribe con cortes posteriores y se imprime al arrancar. No se lo
  reinyecta como una medición nueva: falta una API para reenviar con su timestamp
  original y liberar el registro al confirmarlo. Esta función requiere acordar
  con la cátedra la continuación de [#10](https://github.com/Universidad-Nacional-de-Rafaela/aura-firmware/issues/10)
  para ESP-IDF; no se presenta como resuelta de extremo a extremo.
- **Reintentos:** se usan los oficiales, con espera creciente de 15 s a 5 min y
  azar. No hay ajuste remoto de `r_s` ni un modo propio de exactamente tres
  intentos por lectura. La detección/reconexión de la mesh la maneja la biblioteca.
- **Alimentación dinámica:** la API recibe `alimentacion` solo al iniciar.
  Se declara `desconocida` en status y los cambios viajan como alertas `energia`;
  falta un setter público para actualizar también el status. No se modifica
  el estado interno de la biblioteca.
- **Tamaño del reporte:** la biblioteca actual conserva `fw` y la configuración
  completa y, si supera los **180 bytes**, retira primero diagnósticos accesorios.
  Esto reemplaza el comportamiento anterior, que descartaba directamente el
  reporte de los ajustes iniciales. El límite sigue existiendo: si ni quitando
  accesorios entran todos los campos, el evento no se envía; un resultado de
  comando grande también puede salir sin `config`. Verificar
  `status.details.config` en el banco, especialmente con nombres de aula largos,
  parámetros extremos y diagnósticos de flash/eventos pendientes. El dispositivo
  no oculta ajustes ni inventa otro tópico para sortear el límite.

## Compilar y cargar

1. Instalar **ESP-IDF 6.1** con [EIM](https://dl.espressif.com/dl/eim/) y abrir
   su terminal con el entorno activado. El IDE de Arduino no compila esta versión.
2. Desde la raíz del repositorio:

   ```powershell
   cd dispositivos/E1-PB-LECA-HFR01
   Copy-Item main/config_local.ejemplo.h main/config_local.h
   ```

3. Completar **solo** `MESH_ID_BYTES` y `MESH_AP_PASS` con los datos de la
   cátedra. No hace falta SSID, contraseña del WiFi, broker, UUID ni aula.
   `main/config_local.h` está ignorado por Git; no subirlo.
4. Revisar `main/hardware.h` contra el circuito real. Los componentes externos
   se descargan automáticamente con sus versiones declaradas.
5. En esa carpeta:

   ```powershell
   idf.py set-target esp32c3
   idf.py -p COM5 build flash monitor
   ```

   Cambiar `COM5` por el puerto real. Salir del monitor con `Ctrl+]`.
   Sin datos de mesh válidos o sin NVS, el programa conserva diagnóstico local,
   pero no promete guardar mediciones en la cola: revisar los errores del monitor.

La tabla es la de `Mesh3_Hoja`: `otadata`, dos aplicaciones `ota_0`/`ota_1` de
1,75 MiB y `cola` de 256 KiB, en una flash de 4 MiB. La distribución cambia
respecto de Arduino y de la primera versión ESP-IDF sin OTA; no se migran colas
experimentales anteriores, porque ninguna versión previa llegó a instalarse.
No usar `erase-flash` como paso rutinario: borra ajustes, corte protegido y datos
pendientes.

Si ya se compiló con los ajustes anteriores, regenerar la configuración y los
resultados locales antes de compilar: `sdkconfig` y `build/` son generados e
ignorados por Git. No borrar `main/config_local.h`. No alcanza con reemplazar
`sdkconfig.defaults` y reutilizar un `sdkconfig` viejo con rollback desactivado.

## Actualización del firmware por la malla (OTA)

La primera instalación de esta distribución de memoria se hace **por USB**,
incluyendo bootloader y tabla de particiones (`idf.py -p COM5 flash`). Después,
la biblioteca de la cátedra recibe las actualizaciones a través de la mesh;
no se agrega un comando OTA propio ni se conecta la hoja al WiFi del edificio.

El proyecto declara `project(aura-heladera-freezer)` y `PROJECT_VER = 1.0.0`
en `CMakeLists.txt`. La imagen es `build/aura-heladera-freezer.bin`. El tipo
describe el uso, no la ubicación ni una placa particular. Se informa como
`details.firmware.tipo` y `details.firmware.version` en AURA. Antes de cada
entrega hay que subir la versión: por ejemplo, `1.0.1` para una corrección.
La actualización a la misma versión responde `sin_cambios`.

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` habilita la vuelta atrás: una versión
nueva se confirma cuando se comunica con la raíz y acumula 60 s estable. Si no
se confirma en 10 min, o reinicia antes de confirmarse, puede volver a la
anterior. La transferencia, comprobación de imagen, respaldo de la cola y
confirmación los implementa la biblioteca compartida.

Para el banco usar las herramientas oficiales y el procedimiento de
[la guía de nodos hoja](../../Mesh_Red_Completa/GUIA_NODOS_HOJA.md#28-actualización-por-la-malla-ota)
y [pruebas OTA y cortes](../../Mesh_Red_Completa/PRUEBAS_OTA_Y_CORTES.md).
La transferencia y el rollback físicos todavía no están validados en este nodo.

## Pruebas

En computadora, desde la raíz del repositorio:

```powershell
python dispositivos/E1-PB-LECA-HFR01/tests/probar.py
python -m unittest discover -s conectores/E1-PB-LECA-HFR01/tests -v
python herramientas/validar_dispositivos.py
python herramientas/validar_conectores.py
```

Validación de configuración del 26/10/2026: compilación completa con
**ESP-IDF 6.1.0**, el `main` actualizado de la cátedra y parámetros de mesh
ficticios, sin flashear. La imagen `aura-heladera-freezer` versión `1.0.0`,
para `esp32c3`, ocupa **1.151.328 bytes**, con **37 % libre** en cada partición
OTA de 1,75 MiB. El lector de imágenes de `Broker_MQTT/ota.py` acepta el tipo,
la versión y el chip. Se comprobó rollback activado en la configuración generada.

En la revisión D5 del mismo día pasaron los 20 tests del conector, los 39 del
núcleo del SDK y ambos validadores, y el archivo de ejemplo produjo los
resultados esperados. La lógica de medición del nodo conserva las pruebas de
host aprobadas el 20/10: no se modificó al adaptar la configuración OTA.
La prueba aislada en Linux/WSL y las pruebas físicas de transferencia y rollback
quedan pendientes; compilar y leer la imagen no las reemplaza.

Las pruebas C requieren un compilador (`cc`, `gcc`, `clang` o `--cc "zig cc"`).
La primera ejecución descarga cJSON 1.7.19 oficial y comprueba SHA256. Cubren
parches inválidos, límites, atomicidad, fallos de escritura/commit NVS, CRC,
centinelas de sonda, alarma y calendario del DS3231. No simulan la radio ni
demuestran la entrega real de la cola de la cátedra.

En banco, con el raíz y el concentrador actualizados y el alta hecha por la cátedra:

1. Conectar la C3 por USB a la computadora y verificar arranque, hora y ambas
   sondas; si falta una, debe omitirse y aparecer su alerta. Verificar OLED y LEDs.
2. Probar intervalos distintos y umbrales distintos. Cambiar un umbral debe
   afectar al LED local aun sin red. Probar un parche inválido: no cambia nada.
   Reiniciar y comprobar que los ajustes válidos se conservaron.
3. Observar `hw/<hw_id>/data`, `status`, `response`, `ack` y `alerts/...` con las
   herramientas/credenciales del banco. La configuración completa de status es
   una verificación pendiente por el límite de tamaño explicado arriba.
4. Cortar AURA/broker más de 5 min, dejar acumular pendientes y recuperar.
   Verificar hora original, ingest_id y retiro únicamente tras ACK. Probar
   reinicio con datos ya respaldados y documentar pérdidas en RAM/overflow.
5. Con el circuito de alimentación confirmado, habilitar su GPIO y simular
   red→batería→red. Debe apagarse OLED, capturarse el primer corte y conservarse
   al reiniciar, incluso sin sondas. Su reenvío histórico queda pendiente de API.
6. Probar el cambio de concentrador y la asignación de aula desde AURA. No
   fijar el aula en el binario. Si la alimentación externa impide asociar la
   C3, repetir con USB de computadora: la cátedra informó un problema de antena.
7. Después de la primera carga por USB, probar una actualización por la malla
   con una versión superior y comprobar `aplicado` y `details.firmware`.
   Seguir el protocolo de la cátedra para verificar el rollback; no darlo por
   probado solamente porque la imagen compila.

Quedan por confirmar cableado C3, circuito red/batería, datos privados de mesh,
alta/asignación en AURA y las extensiones de API indicadas. Autonomía de batería,
vida útil real de flash y comportamiento físico aún no están medidos.
