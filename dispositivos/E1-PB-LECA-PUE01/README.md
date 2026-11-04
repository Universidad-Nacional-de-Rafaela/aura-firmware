# E1-PB-LECA-PUE01 — puerta con tarjeta RFID y cerradura eléctrica

Control de acceso de una puerta: abre con tarjeta, vigila la apertura con un sensor de puerta y
tiene un modo armado (alarma) y un modo emergencia. Es un **prototipo en banco**: todavía no habla
con AURA.

| | |
|---|---|
| **Ubicación** | Edificio 1, planta baja, LabECA (a confirmar con la cátedra) |
| **Responsable** | Grupo de IC IV 2026 (losHijosDelStackOverFlow) |
| **Estado** | en banco: probado en placa, sin instalar |
| **Instalado** | todavía no |
| **Transporte** | ESP-NOW punto a punto con la enroladora ([`herramientas/enrolador_puerta/`](../../herramientas/enrolador_puerta/)). **No está en la mesh ni en AURA todavía**: queda pendiente portarlo a ESP-IDF sobre la mesh (ver [ADR-007](../../docs/adr/ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md)) |
| **Contrato que implementa** | ninguno todavía. El banco simula las órdenes `abrir` y `emergencia` con el formato de `hw/<hw_id>/command` y `/response` de [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) |
| **Basado en** | ninguno |
| **Códigos anteriores** | ninguno |

## Qué hace

- **Tarjeta autorizada**: el pestillo se abre 10 s con un pi pi pi que sube de volumen y de ritmo, y se
  vuelve a trabar. Si la puerta se abre y a los 15 s sigue abierta, suena un aviso cada vez más
  fuerte hasta que se cierre. Si se abre y se cierra dentro de la ventana, el pestillo se traba antes.
- **Tarjeta no autorizada**: dos pitidos graves y no pasa nada más.
- **Desarmado** (estado normal): se puede abrir la puerta desde adentro sin que suene nada.
- **Armar**: dos pasadas de tarjeta seguidas, la segunda mientras el pestillo sigue abierto por la
  primera. Hay 10 s con un pitido por segundo para salir y después queda armado.
- **Armado**: si la puerta se abre sin tarjeta, suena la sirena. Una pasada de tarjeta autorizada
  la corta, desarma el sistema y abre el pestillo para entrar. El estado armado se guarda en NVS y
  sobrevive a un corte de luz.
- **Modo emergencia** (orden `emergencia` con `params.activa`): el pestillo queda abierto de forma
  permanente y suena una alarma de dos tonos lentos hasta que se apague. Gana a todo lo demás, deja
  el sistema desarmado y también se guarda en NVS.
- **Orden `abrir` de la enroladora**: cuenta como una tarjeta autorizada y el monitor serie la
  marca como `(app)`.
- La lista de tarjetas (hasta 20, más una maestra fija en el código) se administra desde la
  enroladora y se guarda en NVS.

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | ESP32-C3 SuperMini | — |
| Lector RFID | RC522 | SCK 9, MISO 10, MOSI 20, SS 21, RST 7 (3,3 V) |
| Buzzer pasivo | directo al pin | GPIO 3 |
| Sensor de puerta | botón entre GPIO 4 y GND (pull-up interna), **simula el reed**: apretado = abierta. Con el reed real, poner `NIVEL_PUERTA_ABIERTA` en `HIGH` | GPIO 4 |
| Módulo de relé | 1 canal, 5 V, activo en `LOW` | IN al GPIO 5, VCC al pin 5V, GND |
| Cerradura | eléctrica Luber 500, 8–12 V, 5 W, **fail-safe** | contactos del relé |
| Fuente | 12 V, 1 A o más, aparte | COM del relé; el GND de la fuente **no** se une al del ESP |
| Diodo | 1N4007 (o 1N4001 / 1N5819) | en paralelo con la cerradura, con la **banda del lado +** |

Cableado de potencia: fuente + → COM del relé; NO del relé → + de la cerradura; − de la cerradura
→ fuente −.

**El diodo no es opcional.** Al soltar el relé, la bobina de la cerradura larga un pico de tensión
que, sin diodo, colgaba el ESP y le tiraba abajo el USB (el buzzer quedaba clavado en un tono).

### La cerradura es fail-safe

Con corriente queda **trabada** y sin corriente se **abre**: relé activado = trabada, relé suelto =
abierta. Consecuencias:

- Está energizada casi todo el tiempo (5 W continuos): conviene revisar que no se caliente.
- En un corte de luz, o durante los ~2 s del arranque de la placa, queda **abierta**.
- El modo emergencia no gasta corriente: la deja sin energizar.

Con una cerradura fail-secure habría que invertir la lógica en `actualizarCerradura()`.

## Lo que envía a AURA

Nada todavía. Las órdenes llegan por ESP-NOW desde la enroladora y el estado se ve por el monitor
serie.

## Configuración

Hoy los tiempos están fijos en el código (no hay `set_config`):

| Parámetro | Valor | Constante |
|---|---|---|
| Pestillo abierto tras una tarjeta | 10 s | `VENTANA_ACCESO_MS` |
| Silencio con la puerta abierta antes del aviso | 15 s | `ESPERA_AVISO_MS` |
| Cuenta para salir al armar | 10 s | `TIEMPO_ARMADO_MS` |

Pendiente: hacerlos configurables desde AURA (con rango validado y guardado en NVS) y gestionar
las tarjetas desde la app.

## Compilar y probar

1. Copiar `config_local.h.example` como `config_local.h` y completarlo con la MAC de la enroladora
   y las claves de ESP-NOW (las mismas que en la enroladora). Sin él el sketch compila y vigila la
   puerta, pero no acepta órdenes de la enroladora.
2. Bibliotecas: las de `bibliotecas.txt`.
3. Placa `ESP32C3 Dev Module` o `Nologo ESP32C3 Super Mini` con `CDCOnBoot=cdc` (si no, el monitor
   serie queda mudo).
4. En el monitor serie (115200) se ve cada decisión: `ACCESO OK`, `Cerradura: ABIERTA/CERRADA`,
   `SISTEMA ARMADO`, `ALARMA: apertura forzada`, etc. Pruebas básicas: pasar la tarjeta (el relé
   hace clic y el pestillo se abre 10 s), apretar el botón con el sistema armado (suena la sirena),
   y mandar `abrir` y `emergencia` desde la enroladora.

## Notas

- El sonido usa LEDC directo (`ledcAttach`/`ledcWrite`); no mezclar con `tone()`.
- La antena del RC522 se reinicia de a ratos (el sketch lo detecta y la reinicializa cada 5 s
  como máximo); no se investigó la causa, posiblemente alimentación o cableado.
- Las tarjetas se agregan y borran con la enroladora; la maestra está fija en `UID_MAESTRA`.
