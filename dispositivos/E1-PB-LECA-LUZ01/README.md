# E1-PB-LECA-LUZ01 — control de iluminación del aula del LabECA

Mide la presencia con un radar y enciende o apaga la luz del aula **localmente**: la lógica
vive en el firmware, AURA no decide en cada ciclo. Si la luz del aula (y la que entra por la
ventana) ya alcanza, no enciende. Publica presencia, lux, temperatura/humedad y estado del
relé por la mesh, y cumple el contrato v4.0 (ack antes de borrar de la cola).

| | |
|---|---|
| **Ubicación** | Edificio 1, planta baja, LabECA |
| **Responsable** | Grupo de IC IV 2026 |
| **Estado** | firmware completo: LD2420 + LED de presencia probados; BH1750 ×2, SHT30 y relé por cablear/probar |
| **Instalado** | todavía no |
| **Transporte** | mesh ESP-WIFI-MESH, según [ADR-006](../../docs/adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md) (nodo hoja) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.0 |
| **Basado en** | [`../../ejemplos/plantilla_dispositivo/`](../../ejemplos/plantilla_dispositivo/) |
| **Códigos anteriores** | ninguno |

**Identidad**: por MAC de la placa (`hw_id` `mac-…`), como todo dispositivo del repo
([ADR-005](../../docs/adr/ADR-005-identidad-por-placa-y-mapeo-en-aura.md)).

## Hardware

| Parte | Qué hace | Pines (defaults) |
|---|---|---|
| ESP32-C3 (SuperMini o XIAO) | la placa | IDE: placa **ESP32C3 Dev Module** o **XIAO_ESP32C3**, USB CDC On Boot: Enabled. No usar 18/19 (USB), 11..17 (flash), 2/8/9 (strapping) |
| LD2420 | radar mmWave de presencia, UART1 | `OT1` (TX del sensor) → **GPIO7**, `RX` del sensor ← **GPIO6**, 3V3 y GND. El **TX del módulo es OT1** (firmware ≤ 1.5.2 lo trae invertido: probar `LD2420_BAUD 256000`). Alimentación **3V3** (3.0..3.6 V), nunca 5 V |
| LED de presencia | indica gente en la ventana | **GPIO1** → 220–330 Ω → LED → GND. Prende mientras la distancia reportada esté en la ventana y el radar conteste |
| BH1750 ventana + aula | lux por I2C | SDA **GPIO4**, SCL **GPIO5**; ventana 0x23 (ADDR a GND), aula 0x5C (ADDR a 3V3) |
| SHT30 | temperatura y humedad | mismo I2C, 0x44 |
| Relé (1 canal) | luz del aula | IN → **GPIO0**; DC+ a 3V3 (03VDC) o 5V (05VDC), DC- a GND. GPIO0 es strapping: si la placa no arranca con el relé, pasar IN a GPIO10 y cambiar `RELE_PIN` |

**Datos del datasheet** (`datasheet/hlk-ld2420.pdf`): alcance 8 m, precisión de distancia
**±0.35 m**, puertas de **70 cm** (0..15), y **solo informa distancia de cuerpos en
movimiento** (sin movimiento queda el último valor y la presencia se cae).

**Ventana de distancia**: la presencia solo cuenta si el radar la mide entre **`DIST_MIN_CM`
y `DIST_MAX_CM`** (20..400 cm, hasta ~4 m) y se mantiene hasta **0..420 cm** (histéresis
`DIST_HIST_CM`). Constantes en el `.ino`. Al arrancar se imprime la config grabada del
sensor; cada cambio de estado imprime
`[RADAR] movimiento, distancia 250 cm (entra 20..400, sale 0..420) -> PRESENCIA`.

## La lógica local

```
sin gente (reposo, tras delay_off_s)     → luz OFF
con gente, lux < umbral_bajo_lx (20 lx)  → prender      (BAJO en cualquiera de los dos)
con gente, lux > umbral_alto_lx (40 lx)  → apagar       (ALTO en los dos sensores)
entre umbrales (MEDIO) o un solo ALTO    → mantener     (evita parpadeo)
```

Un cambio tiene que sostenerse **2 s** (debounce) antes de commutar el relé. Si un sensor de
luz falla, decide el otro; si fallan los dos, mantiene. `delay_off_s` (10 s) sostiene el
sistema ese rato después de que el radar deja de ver a nadie. Si el radar no responde, **no
se publica `presencia`** (nada de valores inventados) y va la alerta `sensor`. Si un sensor
de luz o el SHT30 fallan, su campo desaparece de la muestra y su sonda avisa.

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
| `umbral_bajo_lx` | 20 | 1..20000 | por debajo de esto = BAJO (prender); tiene que ser menor que `umbral_alto_lx` |
| `umbral_alto_lx` | 40 | 1..20000 | por encima = ALTO (apagar) |
| `delay_off_s` | 10 | 0..3600 | cuánto sostiene el sistema al perder la presencia |

No hay comandos propios (`ejecutar_comando` es NULL): la luz la manda solo la lógica local.

## Conector

En [`../../conectores/E1-PB-LECA-LUZ01/`](../../conectores/E1-PB-LECA-LUZ01/): guarda la
serie y alerta una sola vez por cruce de presencia ("Aula ocupada" / "Aula libre"). Se prueba
en la PC: `python3 -m unittest discover -s conectores/E1-PB-LECA-LUZ01/tests`.

## Compilar y probar

```bash
# tests de la lógica (se corren en la PC)
make -C tests test
```

En Arduino IDE: placa **ESP32C3 Dev Module**, *USB CDC On Boot: Enabled*, bibliotecas de
`bibliotecas.txt`, y copiar `config_local.h.example` → `config_local.h` (pines por defecto
los de arriba; los datos de la mesh los da la cátedra). Sin `config_local.h` compila igual,
pero no envía (guarda en la cola). El sketch usa ~97% de la partición por defecto (1.3 MB):
si la IDE no deja compilar algo más, subir *Partition Scheme* a **No OTA (2 MB APP)**.

Para darlo de alta en AURA hay que agregar la fila a la tabla *Dispositivos* de
[`../../dispositivos/README.md`](../../dispositivos/README.md) y cargar la placa con
`herramientas/leer_mac`; los dos pasos van juntos en el mismo PR.
