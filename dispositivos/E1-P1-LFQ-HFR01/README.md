# E1-P1-LFQ-HFR01 — Monitoreo de heladera

| | |
|---|---|
| **Ubicación** | Edificio 1, primer piso, Laboratorio de Fisicoquímica. |
| **Responsable** | Grupo de IC IV 2026: @VainstubTomas, @Bernidb, @Kinggrass265, @JuanCruz46. |
| **Estado** | Prototipo probado en banco. Adaptación a una sonda pendiente de validación en placa e instalación. |
| **Instalado** | Todavía no. |
| **Transporte** | Interior: hoja ESP-WIFI-MESH; no reenvía tráfico de otras placas. |
| **Contrato** | [Copia MQTT v4.4 (propuesta)](../../docs/CONTRATO_MQTT.md), [ADR-007](../../docs/adr/ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md) y [ADR-008](../../docs/adr/ADR-008-actualizacion-por-la-malla-y-tipo-de-firmware.md). |
| **Basado en** | [Mesh3_Hoja](../../Mesh_Red_Completa/Mesh3_Hoja/) y su biblioteca pública `nodo_mesh`. |
| **Firmware** | Tipo `aura-heladera-freezer`, versión `1.0.3`; mismo binario para las placas del mismo uso. |
| **Códigos anteriores** | `E1-PB-LECA-HFR01`. |

## Qué hace

Una **ESP32-C3 SuperMini** lee una sonda DS18B20 de la heladera, con intervalo
y umbrales configurables. El LED de alarma se enciende después de dos lecturas
válidas seguidas del mismo lado del rango, incluso sin conexión. Una pantalla
SH1106 muestra la temperatura y los pendientes. El DS3231 con pila permite
conservar UTC sin red.

La biblioteca de la cátedra administra la conexión, el concentrador de aula, la
cola híbrida y los ACK de AURA. El dispositivo usa exclusivamente su API pública:
no accede a variables internas ni a `esp_wifi_*` o `esp_mesh_*`.
El conector Python de [conectores/E1-P1-LFQ-HFR01](../../conectores/E1-P1-LFQ-HFR01/)
valida y guarda mediciones, y genera alertas al cruzar umbrales.

## Organización del código

| Archivo o carpeta | Responsabilidad |
|---|---|
| `CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv` | Proyecto ESP-IDF 6.1, C3, tipo/versión, dos particiones OTA, rollback y cola de 256 KiB. |
| `main/main.c` | Arranque, intervalo de medición y API de AURA. |
| `main/hardware.h` | Propuesta de GPIO y opciones de pantalla, RTC y entrada de alimentación. |
| `main/config_local.ejemplo.h` | Plantilla del ID y la clave privados de la mesh. |
| `main/logica.c/.h` | Rangos, alarma con histéresis, CRC y calendario UTC. |
| `main/config_parser.c`, `main/configuracion.c/.h` | Parche JSON, validación completa, aplicación y reporte de configuración. |
| `main/almacenamiento.c/.h` | NVS de configuración y último estado de alimentación. |
| `main/sondas.c/.h` | Bus 1-Wire de heladera, detección y lectura de DS18B20 con el controlador de Espressif. |
| `main/bus_i2c.c/.h` | Bus I2C compartido por OLED y RTC. |
| `main/reloj.c/.h` | DS3231, validación de la hora y reloj de sistema. |
| `main/pantalla.c/.h` | SH1106 128×64 y LEDs externos de umbral y desconexión. |
| `main/energia.c/.h` | Antirrebote y alerta por cambio de alimentación. |
| `main/idf_component.yml`, `dependencies.lock` | Dependencias declaradas y versiones resueltas por ESP-IDF. |
| `tests/` | Pruebas de lógica y configuración en computadora; no se cargan en la placa. |

## Hardware

Los siguientes GPIO son una **propuesta nueva para la C3**, no una equivalencia
de los D0…D5 de la XIAO. Confirmar cableado y niveles antes de energizar.

| Componente | Conexión inicial en `main/hardware.h` |
|---|---|
| ESP32-C3 SuperMini | Flash de 4 MiB; placa provista por el proyecto. |
| DS18B20 heladera | GPIO3, bus independiente con pull-up externo de 4,7 kΩ a 3,3 V. |
| LED umbral, externo con resistencia | GPIO7, activo alto. |
| LED desconexión, externo con resistencia | GPIO10, activo alto. |
| OLED SH1106 128×64 | SDA GPIO8, SCL GPIO9; dirección 0x3c. |
| RTC DS3231 con pila | Mismo I2C, dirección 0x68; hora guardada en UTC. |
| Detección de alimentación | Deshabilitada (`HFR_PIN_ALIMENTACION = -1`) hasta confirmar el circuito. |

Todos los GPIO trabajan a 3,3 V. El bus I2C utiliza pull-ups a 3,3 V.
La sonda se usa con alimentación externa y tiene un pull-up de 4,7 kΩ.
La resolución es de 9 bits (0,5 °C) y cada conversión ocupa unos 100 ms.

### I2C y carga por USB del primer prototipo

Este prototipo utiliza los pines marcados SDA y SCL de la SuperMini, **GPIO8 y
GPIO9**. GPIO9 también está conectado al botón **BOOT**. En las pruebas de
banco la OLED y el RTC funcionaron con este cableado. También se pudo conectar
la placa por USB a una PC y abrir el monitor serie durante el uso.

Para **grabar firmware por USB**, en este montaje fue necesario desconectar
el bus I2C de la placa, en particular SCL de GPIO9. Una vez terminada la carga,
cortar la alimentación, reconectar el bus y reiniciar. La próxima grabación
del prototipo la realiza la cátedra con su firma.

GPIO8 y GPIO9 intervienen en el arranque. Si un módulo mantiene GPIO9 en bajo
al reiniciar, la placa puede entrar en modo descarga y dejar de medir.
Pulsar BOOT en marcha también baja SCL. El funcionamiento observado en banco
no cubre una falla del bus durante un reinicio o una actualización OTA.
Al replicar unidades, evaluar y probar otro par de GPIO para I2C.
Reservar GPIO18/19 para USB y tener en cuenta GPIO2 en el arranque.

La alarma se recupera al entrar 0,5 °C en el rango desde el umbral que se
cruzó. Para rangos menores a 1 °C, el margen es la mitad de su ancho. Si una
sonda falla, el LED de temperatura conserva el estado de su alarma. Una falla
por sí sola se indica en pantalla y con la alerta `sensor` en AURA.

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

Cada muestra válida contiene `temp_heladera_c`. Se omiten lecturas fallidas,
no finitas y los centinelas -127 y 85. La falla de sonda genera la alerta
`sensor`. El JSON de la medición queda por debajo del límite de 160 bytes.

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
válida guardada. Si al arrancar la NVS está llena o tiene una versión
incompatible, se borra y se reinicializa como en `Mesh3_Hoja`. Se pierden los
ajustes de esa partición, incluida el aula asignada. Los demás errores
se informan por el monitor serie.

| Parámetro | Significado y rango | Inicial |
|---|---|---|
| `intervalo_s` | Heladera: entero 5…86400 s. También lo usa el raíz para inferir desconexión. | 60 |
| `temperatura_min_c` | Temperatura mínima de heladera, °C, -55…125 y menor que la máxima. | 2 |
| `temperatura_max_c` | Temperatura máxima de heladera, °C, -55…125 y mayor que la mínima. | 6 |
| `aula` | Asignada y guardada por la biblioteca de AURA; el dispositivo no la fija. | Sin asignar |

La configuración reportada muestra `intervalo_s`, `temperatura_min_c` y
`temperatura_max_c`. Los nombres anteriores `hlo` y `hhi` siguen aceptándose
como alias al recibir comandos y al leer el estado anterior desde el conector.
Un parche que repite un mismo umbral usando ambos nombres se rechaza.
Al cargar una configuración NVS del prototipo anterior se recuperan el intervalo
y los umbrales de heladera. El siguiente cambio se guarda en el formato nuevo.

Los umbrales aceptan hasta dos decimales, aunque la resolución de la sonda es
0,5 °C. Se rechazan tipos incorrectos, nombres desconocidos, claves duplicadas
del dispositivo y rangos invertidos. Los campos ausentes conservan su valor.

```json
{"command":"set_config","params":{"intervalo_s":120,"temperatura_min_c":2,"temperatura_max_c":7},"command_id":"hfr-1"}
```

Usar parches y `command_id` cortos: el comando completo tiene un límite de
180 bytes. La configuración vigente se agrega mediante `describir_config`.

## Cola y cortes

La **única cola de mediciones** es la de la cátedra: hasta 400 registros en RAM
(puede reducirse si no hay memoria), respaldo en flash cuando una muestra lleva
5 minutos sin confirmación, recuperación tras reinicio y descarte de la más vieja
al llenarse. En operación normal las mediciones no escriben flash. Un reinicio
puede perder los últimos 5 minutos que todavía estaban solo en RAM; no equivale
a persistencia inmediata de todas las lecturas.

La entrada de alimentación, cuando está habilitada, reporta transiciones
con la alerta `energia`. La hoja sigue midiendo según los intervalos configurados.
El reporte de estado tiene un límite de 180 bytes. Con valores extremos
de configuración y diagnósticos como `en_flash` o `eventos_perdidos`,
puede superar el límite incluso después del recorte de la biblioteca y no
publicarse. Es una limitación del reporte. La configuración aplicada y
guardada en la placa sigue vigente.

## Compilar y cargar

1. Instalar **ESP-IDF 6.1** con [EIM](https://dl.espressif.com/dl/eim/) y abrir
   su terminal con el entorno activado. El IDE de Arduino no compila esta versión.
2. Desde la raíz del repositorio:

   ```powershell
   cd dispositivos/E1-P1-LFQ-HFR01
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
   idf.py build
   ```

   Para validar una compilación completa, usar datos de mesh válidos o
   parámetros ficticios con formato válido. Los marcadores de la plantilla
   pueden generar un binario de diagnóstico que excluye la mesh.
6. Entregar el código mediante un PR. Después del merge, la cátedra compila
   desde `main` con las credenciales de la instalación, firma y graba el prototipo.
   La carga por USB requiere desconectar el bus I2C como se explica en Hardware.
   Para observar una placa ya cargada, usar `idf.py -p COM5 monitor` con el
   puerto real. Salir con `Ctrl+]`.

La tabla es la de `Mesh3_Hoja`: `otadata`, dos aplicaciones `ota_0`/`ota_1` de
1,75 MiB y `cola` de 256 KiB, en una flash de 4 MiB. Reservar `erase-flash`
para los casos de recuperación que lo requieran, porque borra ajustes y datos
pendientes.

Si ya se compiló con los ajustes anteriores, regenerar la configuración y los
resultados locales antes de compilar: `sdkconfig` y `build/` son generados e
ignorados por Git. No borrar `main/config_local.h`. No alcanza con reemplazar
`sdkconfig.defaults` y reutilizar un `sdkconfig` sin verificación de firma.

## Actualización del firmware por la malla (OTA)

La primera instalación de esta distribución de memoria se hace **por USB**,
incluyendo bootloader y tabla de particiones. Grabar desde el principio
una imagen **firmada con RSA-3072** según [la guía §2.9](../../Mesh_Red_Completa/GUIA_NODOS_HOJA.md#29-imágenes-firmadas).
Una placa que arranca con una imagen sin firma no acepta actualizaciones OTA.

`idf.py build` produce el binario sin firmar. El grupo entrega el código por PR.
Después del merge, la cátedra compila desde `main` con las credenciales de la
instalación y firma esa imagen, según la guía §2.9. La imagen firmada contiene
los datos de la mesh y se distribuye por el canal que indique la cátedra.
La placa verifica la firma al actualizar por la mesh.

El proyecto declara `project(aura-heladera-freezer)` y `PROJECT_VER = 1.0.3`
en `CMakeLists.txt`. El tipo `aura-heladera-freezer` conserva la identidad
de firmware registrada para este prototipo y su compatibilidad con OTA.
La medición, la pantalla y el conector corresponden a una sola sonda de heladera.
La imagen es `build/aura-heladera-freezer.bin`. El tipo
describe el uso, no la ubicación ni una placa particular. Se informa como
`details.firmware.tipo` y `details.firmware.version` en AURA. Incrementar la versión para cada
nueva entrega. La actualización a la misma versión responde `sin_cambios`.

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
python dispositivos/E1-P1-LFQ-HFR01/tests/probar.py
python -m unittest discover -s conectores/E1-P1-LFQ-HFR01/tests -v
python herramientas/validar_dispositivos.py
python herramientas/validar_conectores.py
```

Las pruebas C requieren un compilador (`cc`, `gcc`, `clang` o `--cc "zig cc"`).
La primera ejecución descarga cJSON 1.7.19 oficial y comprueba SHA256. Cubren
parches inválidos, límites, atomicidad, fallos de escritura/commit NVS, CRC,
centinelas de sonda, histéresis, energía y calendario del DS3231. La entrega real
de la cola de la cátedra se verifica en banco.

En banco, con el raíz y el concentrador actualizados y el alta hecha por la cátedra:

1. Conectar la C3 por USB a la computadora y verificar arranque, hora y sonda
   de heladera. Si falta la sonda, comprobar su alerta. Verificar OLED y LEDs.
2. Cambiar el intervalo, la temperatura mínima y la máxima. Comprobar el LED
   local sin red, el rechazo de parches inválidos y la conservación de los ajustes
   tras reiniciar.
3. Observar `hw/<hw_id>/data`, `status`, `response`, `ack` y `alerts/...` con las
   herramientas y credenciales del banco. Probar la configuración completa
   de status con umbrales e intervalos extremos, y con aula asignada.
4. Cortar AURA/broker más de 5 min, dejar acumular pendientes y recuperar.
   Verificar hora original, ingest_id y retiro únicamente tras ACK. Probar
   reinicio con datos ya respaldados y documentar pérdidas en RAM/overflow.
5. Con el circuito de alimentación confirmado, habilitar su GPIO y simular
   red→batería→red más de una vez. Verificar la OLED y las alertas `energia`
   de cada transición.
6. Probar el cambio de concentrador y la asignación de aula desde AURA. No
   fijar el aula en el binario. Si la alimentación externa impide asociar la
   C3, repetir con USB de computadora: la cátedra informó un problema de antena.
7. Después de la primera carga por USB, probar una actualización por la malla
   con una versión superior y comprobar `aplicado` y `details.firmware`.
   Seguir el protocolo de la cátedra para verificar el rollback; no darlo por
   probado solamente porque la imagen compila.

Quedan por confirmar cableado C3, circuito red/batería, datos privados de mesh
y alta/asignación en AURA. Quedan por medir la autonomía de batería, la vida
útil de la flash y el comportamiento físico.
