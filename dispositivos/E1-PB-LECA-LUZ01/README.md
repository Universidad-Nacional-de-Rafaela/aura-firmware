# E1-PB-LECA-LUZ01 — control de iluminación del aula del LabECA

Mide la presencia con un radar y regula la luz del aula **localmente** mandando la luz de la
ventana, con la del aula como respaldo para prender: la lógica vive en el firmware, AURA no
decide en cada ciclo. Sin gente, la apaga tras `delay_off_s`. Publica presencia, lux,
temperatura/humedad y estado del relé por la mesh, y cumple el contrato v4.4 (ack antes de
borrar de la cola). Publica como `presencia` la ocupación (con el retardo de `delay_off_s`), no el
movimiento instantáneo.

| | |
|---|---|
| **Ubicación** | Edificio 1, planta baja, LabECA |
| **Responsable** | Grupo de IC III 2026 |
| **Estado** | firmware completo: LD2420 + LED de presencia probados; BH1750 ×2, SHT30 y relé por cablear/probar |
| **Instalado** | todavía no |
| **Transporte** | mesh ESP-WIFI-MESH, según [ADR-007](../../docs/adr/ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md) (nodo hoja) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.4 |
| **Basado en** | [`../../Mesh_Red_Completa/Mesh3_Hoja/`](../../Mesh_Red_Completa/Mesh3_Hoja/) |
| **Códigos anteriores** | ninguno |

**Identidad**: por MAC de la placa (`hw_id` `mac-…`), como todo dispositivo del repo
([ADR-005](../../docs/adr/ADR-005-identidad-por-placa-y-mapeo-en-aura.md)).

## Hardware

| Parte | Qué hace | Pines (defaults) |
|---|---|---|
| ESP32-C3 (SuperMini o XIAO) | la placa | No usar 18/19 (USB), 11..17 (flash), 2/8/9 (strapping). El resto (incluido GPIO0) se puede usar |
| LD2420 | radar mmWave de presencia, UART1 | `OT1` (TX del sensor) → **GPIO7**, `RX` del sensor ← **GPIO6**, 3V3 y GND. El **TX del módulo es OT1** (firmware ≤ 1.5.2 lo trae invertido: probar `LD2420_BAUD 256000`). Alimentación **3V3** (3.0..3.6 V), nunca 5 V |
| LED de presencia | indica gente en la ventana | **GPIO1** → 220–330 Ω → LED → GND. Prende mientras la distancia reportada esté en la ventana y el radar conteste |
| BH1750 ventana + aula | lux por I2C | SDA **GPIO4**, SCL **GPIO5**; ventana 0x23 (ADDR a GND), aula 0x5C (ADDR a 3V3) |
| SHT30 | temperatura y humedad | mismo I2C, 0x44 |
| Relé (1 canal) | luz del aula | IN → **GPIO0**; DC+ a 3V3 (03VDC) o 5V (05VDC), DC- a GND. GPIO0 no interviene en el arranque del C3; probar el montaje en el banco |

**Datos del fabricante:** alcance 8 m, precisión de distancia
**±0.35 m**, puertas de **70 cm** (0..15), y **solo informa distancia de cuerpos en
movimiento** (sin movimiento queda el último valor y la presencia se cae).

**Ventana de distancia**: la presencia solo cuenta si el radar la mide entre **`DIST_MIN_CM`
y `DIST_MAX_CM`** (20..400 cm, hasta ~4 m) y se mantiene hasta **0..420 cm** (histéresis
`DIST_HIST_CM`). Constantes en `main/main.c`. Cada cambio de estado imprime
`[RADAR] movimiento, distancia 250 cm (entra 20..400, sale 0..420) -> PRESENCIA`.

## La lógica local

```
sin gente (reposo, tras delay_off_s)                  → luz OFF
con gente, ventana > 100 lx                            → luz OFF
con gente, ventana entre 60 y 100 lx                   → mantener
con gente, ventana < 60 lx y foco ya prendido          → mantener
con gente, ventana < 60 lx, foco apagado y aula < 100  → luz ON
con gente, ventana < 60 lx, foco apagado y aula >= 100 → mantener OFF
sin sensor de ventana: aula < 60 lx y foco apagado     → luz ON
si fallan ambos sensores de luz                        → mantener
```

Manda la **ventana** porque el sensor del aula ve la luz del propio foco. Si la ventana está
baja y el foco ya está prendido, se mantiene aunque el aula se ilumine. Si falla la ventana,
el aula solo puede prender; si fallan ambas, se mantiene. Un cambio tiene que sostenerse
**2 s** (debounce) antes de commutar el relé. `delay_off_s` (300 s = 5 min) sostiene la luz
ese rato después de que el radar deja de ver a nadie; el LD2420 informa movimiento, por eso
el default es de minutos. Si el radar no responde, **no se publica
`presencia`** (nada de valores inventados) y va la alerta `sensor`. Si un sensor de luz o el
SHT30 fallan, su campo desaparece de la muestra y su sonda avisa.

## Lo que envía a AURA

| Campo | Unidad | Rango | Cuándo |
|---|---|---|---|
| `presencia` | 0/1 | 0..1 | siempre que el radar contesta |
| `luz_ventana_lx` | lx | 0..65535 | si el BH1750 de la ventana responde |
| `luz_aula_lx` | lx | 0..65535 | si el BH1750 del aula responde |
| `temp_c` | °C | -40..85 | si el SHT30 responde |
| `humedad_pct` | % | 0..100 | si el SHT30 responde |
| `rele_canal1` | 0/1 | 0..1 | siempre (es nuestro propio estado) |

Sale cada `intervalo_s` y en cada **cambio de presencia o de relé** (para que la alerta de
cruce del conector llegue al instante). **Alertas**: `sensor` cuando falla o se recupera un
campo con sonda (motivo `sin_respuesta`). Sonda en falla ⇒ el campo desaparece de `values`,
nunca manda un centinela. Cada muestra espera el **ack** de AURA antes de salir de la cola en
flash (`nodo_mesh.h`).

## Cómo se configura (`set_config`)

Todo o nada: si un parámetro no valida, no se aplica ninguno y el resultado trae el motivo.
Vigente tras reinicios (flash).

| Parámetro | Default | Rango | Qué hace |
|---|---|---|---|
| `intervalo_s` | 60 | 5..86400 | cada cuánto manda medición |
| `umbral_bajo_lx` | 60 | 1..20000 | ventana por debajo = noche; habilita prender |
| `umbral_alto_lx` | 100 | 1..20000 | ventana por encima = día; apaga |
| `delay_off_s` | 300 | 1..3600 | cuánto sostiene el sistema al perder la presencia (min 1) |

No hay comandos propios (`ejecutar_comando` es NULL): la luz la manda solo la lógica local.

## Notas de diseño

- Al encender, `sistema_activo` arranca en `true`, por lo que durante `delay_off_s` (5 min) se
  publica `presencia: 1` y puede prender la luz aunque el aula esté vacía. Es intencional:
  al encender las luminarias quedan disponibles mientras el radar confirma la ocupación.
- Si el radar se cae, el sistema pasa a reposo y apaga la luz aunque haya gente adentro: es
  decisión de diseño (sin presencia confirmada no se mantiene encendida).
- Condición de montaje: el sensor de la ventana debe mirar hacia afuera, sin luz directa del
  foco. Si le llegan más de 40 lx del foco (umbral_alto - umbral_bajo), la luz puede oscilar.
- Confirmar en el banco que con el aula vacía el radar manda `OFF` al menos una vez por
  segundo; si no, queda stale (`stale_after_ms = 1000`) y sale la alerta `sensor`.

## Conector

En [`../../conectores/E1-PB-LECA-LUZ01/`](../../conectores/E1-PB-LECA-LUZ01/): guarda la
serie y alerta una sola vez por cruce de presencia ("Aula ocupada" / "Aula libre"). Se prueba
en la PC: `python3 -m unittest discover -s conectores/E1-PB-LECA-LUZ01/tests`.

## Compilar y probar

Firmware (ESP-IDF 6.1, instalado con EIM; ver `Mesh_Red_Completa/GUIA_NODOS_HOJA.md`):

1. Copiar `main/config_local.ejemplo.h` como `main/config_local.h` y completar el ID y la clave
   de la mesh, que los da la cátedra. Sin ese archivo no compila (a propósito).
2. Desde esta carpeta: `idf.py set-target esp32c3` y `idf.py -p <puerto> build flash monitor`.
3. Si cambiás `sdkconfig.defaults` o `partitions.csv`, borrá `sdkconfig` y `build/` antes de
   compilar.

El alta en AURA la hace la cátedra; no hace falta `leer_mac` ni editar la tabla.
