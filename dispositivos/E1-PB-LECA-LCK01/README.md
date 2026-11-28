# E1-PB-LECA-LCK01 — gabinete de lockers del pañol (LabECA)

| | |
|---|---|
| **Ubicación** | Edificio 1, planta baja, LabECA — gabinete de lockers del pañol (4 lockers) |
| **Responsable** | Grupo de lockers del pañol, IC III 2026: MatuteBona, agusferrero05, bertolaco |
| **Estado** | en desarrollo |
| **Instalado** | todavía no |
| **Transporte** | mesh ESP-WIFI-MESH (interior), en ESP-IDF 6.1, según [ADR-007](../../docs/adr/ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md); actualización por la malla según ADR-008 |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.7 (propuesta en este mismo PR): `abrir`, `estado` y las alertas de la puerta de la v4.5, con varias puertas en un dispositivo (§3.1, §3.2, §3.3 y §3.5) |
| **Decisiones** | [ADR-009](../../docs/adr/ADR-009-puertas-con-llavero-la-puerta-decide-y-aura-supervisa.md): cada locker es una puerta con cerradura por pulso y sensor de apertura |
| **Basado en** | [`Mesh_Red_Completa/Mesh3_Hoja`](../../Mesh_Red_Completa/Mesh3_Hoja/), sobre la biblioteca `nodo_mesh`. El pulso (`cerradura_pulso.h`), la salida con corte por temporizador y la validación de `set_config` vienen del modelo [`_modelos/PUE-RFID`](../_modelos/PUE-RFID/) |
| **Firmware** | tipo `aura-lockers`, versión `1.2.0` |
| **Códigos anteriores** | ninguno |

## Qué hace

Supervisa el gabinete de lockers del pañol automatizado: **una placa para los 4 lockers**. Para
cada uno informa en cada cambio si la puerta está abierta o cerrada (su microswitch) y si su
cerradura está energizada, y manda alertas con el número de locker: si se abre sin orden
(`intrusion`), si la placa arranca con él abierto (`arranque_puerta_abierta`), si queda abierto
demasiado tiempo (`puerta_abierta`) y cuando se cierra después de alguna de esas (`puerta_cerrada`).

**La apertura remota está deshabilitada** (ADR-009, decisión 8). Las reglas del préstamo
(reservas, código de apertura, ventana de retiro, mora) viven en el sistema de lockers del grupo,
que pediría la apertura a través de AURA. Hoy AURA manda comandos solo desde la sesión de una
persona y no hay una cuenta para que otro sistema los mande, y la mesh todavía no autentica los
comandos (etapa 2 de seguridad). Mientras tanto, el firmware se compila con `abrir` apagado: se
rechaza con `abrir_deshabilitado`, y el dispositivo solo informa el estado y manda las alertas.
La lógica de `abrir` (con puerta, vencimiento, tope, límite por minuto y un pulso a la vez) está
escrita y probada, y se enciende solo en el banco (ver [Opciones al compilar](#opciones-al-compilar)).

## Hardware y conexionado

Una placa y una fuente para todo el gabinete; por cada locker, un MOSFET, una cerradura, un diodo y
un microswitch.

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | ESP32-C3 SuperMini (4 MB), una para el gabinete | — |
| Llave de potencia, una por locker | MOSFET IRLZ44N (canal N, nivel lógico, TO-220) | Gate ← su GPIO (tabla de abajo) por una resistencia de **470 Ω**; **10 kΩ entre gate y source**; source → −V de la fuente |
| Cerradura, una por locker | DFRobot FIT0620, 9 a 12 V, 1,5 A nominales (en el banco se midieron 5,4 Ω: unos 2,2 A y 27 W a 12 V) | +12 V → cerradura → drain de su MOSFET |
| Diodo de rueda libre, uno por cerradura | 1N4007 | En paralelo con la cerradura: cátodo (franja) al +12 V, ánodo al drain |
| Sensor de puerta, uno por locker | Microswitch KW12-3 | COM → GND, NO → su GPIO (pull-up interno). NC libre. Puerta cerrada = switch apretado |
| Fuente de las cerraduras | Switching 12 V / 5 A, una para el gabinete | +V → cerraduras y cátodos de los diodos; −V → sources de los MOSFET |
| Alimentación de la placa | 5 V por USB, o step-down 12 → 5 V al pin 5V | Nunca 12 V a la placa |
| LED de estado | El de la SuperMini (GPIO8, activo en bajo) | Fijo: en la malla con aula · 1 Hz: sin aula · 4 Hz: buscando la malla |

**Pines** (`main/pines.h`). **Propuesta, a definir con @miltonpozzo**:

| Locker | Gate del MOSFET | Microswitch |
|---|---|---|
| 1 | GPIO0 | GPIO5 |
| 2 | GPIO1 | GPIO6 |
| 3 | GPIO3 | GPIO7 |
| 4 | GPIO10 | GPIO4 |

Lo que tiene que cumplir el conexionado: **ninguna cerradura se destraba mientras la placa
arranca, ni con la placa sin firmware.** Por eso los gates van en pines sin función de arranque (no
GPIO2, 8 ni 9) ni en GPIO21, que transmite la UART del bootloader al arrancar; cada gate tiene su
pull-down de 10 kΩ, que deja el MOSFET apagado mientras el pin está en alta impedancia; y el
firmware pone los cuatro pines en 0 antes que cualquier otra cosa. Falta medirlo con el tester en
cada gate: 0 V durante el arranque y con la placa borrada (`erase-flash`). El locker 1 está armado
y probado; los otros tres se montan con el mismo circuito.

Un microswitch sin conectar se lee como puerta abierta: en el banco, con un solo locker montado, los
pines de los otros microswitches van a GND con un cable, para que se lean cerrados.

Tres cosas que salieron en el banco y no son opcionales:

- **Masa común:** el GND de la placa va unido al −V de la fuente de 12 V (a los sources de los
  MOSFET) con un cable directo. Sin ella, la cerradura no abre y el microswitch lee la puerta
  abierta en el momento del pulso.
- **Pull-down de 10 kΩ en cada gate:** sin él, mientras la placa arranca el gate queda flotando,
  el MOSFET conduce a medias (calienta) y la cerradura se destraba sola.
- **Resistencia en serie de 470 Ω (no de 10 kΩ):** con 10 kΩ en serie y 10 kΩ a GND, al gate
  llegan 1,6 V y el MOSFET no conduce. Con 470 Ω llegan 3,15 V.

El camino de potencia (2,2 A) va por cables o borneras, no por las filas de un protoboard.

### Protección de las bobinas y de la fuente

El fabricante de la FIT0620 pide no energizarla más de 5 s seguidos:
*"you'd better control the electrify time within 5 seconds when unlocking the lock"*
([DFRobot, FIT0620, Reference](https://wiki.dfrobot.com/fit0620/docs/18115)). La hoja no da un
ciclo de trabajo. Por eso:

| | Valor | Dónde |
|---|---|---|
| Tope de un pulso | **5 s**, ni por `set_config` ni por una orden | `cerradura_pulso.h` (`PULSO_TOPE_MS`) y, aparte, `salida_cerradura.c` |
| Pulso por defecto | 3 s (`pulso_ms`, de 0,5 a 5 s) | `set_config` |
| Límite por minuto | 20 s energizada en cualquier ventana de 60 s (ADR-009, decisión 1), **por cerradura**: cada bobina se calienta por su cuenta. Al llegar, alerta `limite_pulsos` | `cerradura_pulso.h`, una `cerradura_t` por locker |
| Un pulso a la vez en el gabinete | La fuente es una, de 5 A, y cada cerradura consume unos 2,2 A. Un `abrir` con **cualquier** cerradura energizada se rechaza con `ocupada`, aunque sea para otro locker. Tampoco se extiende un pulso en curso | `locker_logica.h` |
| Corte del pulso | un `esp_timer` por locker, que no depende de la tarea de la lógica; la tarea está suscrita al watchdog (5 s, con reinicio) | `salida_cerradura.c`, `sdkconfig.defaults` |
| Cortar al abrir | apenas el microswitch de un locker marca abierto, se corta su pulso. Lo comprobamos en nuestra cerradura: **sin energía, la puerta se cierra empujándola y queda trabada**, así que no hace falta seguir energizando | Kconfig, encendido por defecto |

Con pulsos de 3 s que se cortan al abrir, la espera para abrir otro locker es corta.

## Lo que envía a AURA

Cada medición es un `values` (contrato §3.1, varias puertas en la v4.7). Sale una **en cada
cambio** de una puerta o una cerradura, y una completa cada `intervalo_s`:

```json
{"puerta_1":0,"puerta_2":1,"puerta_3":0,"puerta_4":0,"cerradura_1":0,"cerradura_2":0,"cerradura_3":0,"cerradura_4":0}
```

| Campos | Unidad | Rango válido | Se envía |
|---|---|---|---|
| `puerta_1` a `puerta_4` | estado (1 = abierta, 0 = cerrada) | 0 o 1 | en cada cambio y cada `intervalo_s` |
| `cerradura_1` a `cerradura_4` | estado (1 = energizada, 0 = trabada) | 0 o 1 | en cada cambio y cada `intervalo_s` |

Un microswitch desconectado se lee como abierto, que es el estado seguro. Son los nombres del latido
del modelo `PUE-RFID` (`puerta`, `cerradura`) con el número de locker.

En el reporte (`status`, §3.2), `config` trae además `abiertas`, de solo lectura: una lista con un
0 o un 1 por locker (`[0,1,0,0]`).

**Alertas** (contrato v4.5, §3.5, "Alertas de la puerta con llavero"), una por cambio y por
locker, con su número en `details.puerta` (`{"puerta": 2}`):

| Tipo | `severity` | Cuándo |
|---|---|---|
| `intrusion` | `critical` | el locker se abrió sin un pulso suyo en curso y fuera de los 3 s de gracia que siguen a su pulso (la traba tarda en volver). Sale antes que cualquier otro mensaje del mismo cambio. El pulso de otro locker no cuenta |
| `arranque_puerta_abierta` | `high` | la placa arrancó con ese locker abierto |
| `puerta_abierta` | `warning` | quedó abierto más de `aviso_s` |
| `puerta_cerrada` | `info` | se cerró después de una `intrusion`, un `arranque_puerta_abierta` o un `puerta_abierta` |
| `limite_pulsos` | `warning` | un `abrir` no se cumplió porque esa cerradura llegó a los 20 s energizada en el minuto |

**Por qué `intrusion` es `critical`:** con la apertura remota deshabilitada, ninguna apertura de un
locker es legítima desde el punto de vista de la placa, y un locker abierto sin orden es una
herramienta del pañol que se puede llevar cualquiera. Es el mismo caso que una puerta armada que se
fuerza, y AURA lo tiene que tratar igual.

**Por qué `arranque_puerta_abierta` y no `intrusion`:** si la placa arranca con un locker abierto
no se sabe qué pasó. Alcanza con un corte de luz: se corta, alguien abre el locker con la cerradura
sin energía, vuelve la luz y AURA solo vería la puerta abierta. Un locker está siempre armado, así
que vale la alerta que el contrato tiene para una puerta que arranca armada y abierta.

## Comandos

Los de la puerta del contrato v4.5 (§3.3, "Comandos de la puerta con llavero"), con `puerta` para
un dispositivo de varias puertas (v4.7). Los de llaveros (`tarjeta_*`) no aplican: los lockers no
tienen lector.

| Comando | `params` | Qué hace | Rechazos (`details.motivo`) |
|---|---|---|---|
| `abrir` | `{"puerta": <1 a 4>, "vence": <segundos Unix>}` | **Deshabilitado al compilar:** siempre `abrir_deshabilitado`. Con `CONFIG_LOCKER_ABRIR_REMOTO` (solo banco): un pulso de `pulso_ms` en la cerradura de ese locker | `abrir_deshabilitado`; habilitado: `puerta_invalida` (sin `puerta`, no entera o que no existe), `sin_vencimiento`, `sin_hora_valida`, `orden_vencida`, `vencimiento_muy_lejano` (más de 300 s), `ocupada` (cualquier cerradura del gabinete energizada, o el límite por minuto de esa) |
| `estado` | `{}` | De solo lectura: manda el reporte y una medición en el momento | — |

- **El botón del concentrador no toca los lockers:** una orden `local` que no sea `estado` se
  rechaza con `orden_local_no_admitida`.
- Un comando que no es ninguno de estos: `comando_desconocido`.

Ejemplo, en el banco con la opción encendida:
`{"command":"abrir","params":{"puerta":2,"vence":1764183600},"command_id":"..."}`.

## Configuración

Parámetros que acepta `set_config` (contrato §3.3). Valen para los 4 lockers. Tienen que ser
enteros, se validan todos antes de aplicar alguno y se guardan en NVS en un solo bloque. `aula` la
maneja la biblioteca.

| Parámetro | Rango | Valor por defecto |
|---|---|---|
| `intervalo_s` | 5 a 3600 | 30 |
| `pulso_ms` | 500 a 5000 (el tope de la FIT0620) | 3000 |
| `aviso_s` | `0` (sin aviso) o 5 a 600 | 120 |

Un parámetro desconocido se rechaza con `parametro_desconocido`; uno fuera de rango o no entero
(`150.7`), con `<parámetro>_fuera_de_rango`; un `params` que no es un objeto, con
`params_invalido`. En todos los casos la configuración queda como estaba. Un `intervalo_s` nuevo
rige enseguida.

## Opciones al compilar

En `idf.py menuconfig` → *Gabinete de lockers del panol (E1-PB-LECA-LCK01)*:

| Opción | Por defecto | |
|---|---|---|
| `CONFIG_LOCKER_ABRIR_REMOTO` | **apagada** | Con la opción apagada, `abrir` se rechaza con `abrir_deshabilitado` (ADR-009, decisión 8). Se enciende **solo en el banco** |
| `CONFIG_LOCKER_CORTAR_AL_ABRIR` | encendida | Corta el pulso de un locker apenas su microswitch marca abierto |

Para el banco hay un archivo aparte, `sdkconfig.banco`, que enciende la apertura remota en una
carpeta de compilación separada, sin tocar el `sdkconfig` normal:

```
idf.py -B build-banco -D SDKCONFIG=build-banco/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.banco" build
```

## Compilar, firmar y grabar (banco)

Desde la terminal de ESP-IDF 6.1, en `dispositivos\E1-PB-LECA-LCK01`, con `main\config_local.h`
copiado de `config_local.ejemplo.h` y completado con los datos de la mesh del banco:

```
idf.py set-target esp32c3
idf.py build
python ..\..\Mesh_Red_Completa\Broker_MQTT\firmar.py --clave C:\aura-claves\firma_prueba.pem build\aura-lockers.bin
idf.py -p COMx erase-flash      # solo en la primera grabación
idf.py -p COMx flash monitor
```

La imagen entra con un 39 % libre en los 1,75 MB de `ota_0`.

## Tests

La lógica del gabinete está en headers de C puro y se prueba en la compu (el CI los corre):

```
make -C dispositivos/E1-PB-LECA-LCK01/tests test
```

| Test | Qué prueba |
|---|---|
| `test_locker` | los 4 lockers: un pulso a la vez en el gabinete (`ocupada` para el mismo locker y para otro), `puerta_invalida`, cortar al abrir, `intrusion` con su número y antes que la medición, el pulso de un locker no habilita a otro, gracia de 3 s, `arranque_puerta_abierta` por locker, `puerta_abierta` una sola vez y `puerta_cerrada`, `aviso_s: 0`, `limite_pulsos` por cerradura, antirrebote, vigencia de `abrir` |
| `test_cerradura` | tope de 5 s y límite de 20 s por minuto con pulsos de la FIT0620 |
| `test_config` | enteros dentro de rango (`150.7` no pasa) |

## Pruebas

Los informes están en [`pruebas/`](pruebas/).
