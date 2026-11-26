# E1-PB-LECA-CRT01 — Cortina automatizada Luflex del LabECA

Control de apertura y cierre de cortina con motor paso a paso NEMA 23, fin de carrera mecánico (microswitch) y sensor digital de luz ambiental BH1750.

| | |
|---|---|
| **Ubicación** | Edificio 1, planta baja, LabECA |
| **Responsable** | Facundo Boide (`Facundo-Boide`) |
| **Estado** | en desarrollo: firmware compilado, firmado y verificado en placa ESP32-C3; falta calibrar recorrido y probar en la red del laboratorio |
| **Instalado** | todavía no |
| **Transporte** | mesh ESP-WIFI-MESH (interior), según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) y [ADR-006](../../docs/adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.4 |
| **Basado en** | `Mesh_Red_Completa/Mesh3_Hoja` |
| **Códigos anteriores** | ninguno |

## Qué hace

Al arrancar calibra el recorrido completo buscando el sensor magnético de fin de carrera (0 %) y mide cada `intervalo_s` segundos la luz ambiental (`luz_lux`), la posición actual (`posicion_pct`), el estado del motor (`estado_motor`) y el modo activo (`modo`). Puede operar en modo manual (`0`) o en dos modos automáticos (`1` y `2`) que abren o cierran la cortina en tres escalones (0 %, 50 % y 100 %) según dos umbrales de iluminancia con confirmación temporal.

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | ESP32-C3 SuperMini, `hw_id` `mac-907069ab967c` (herramientas/leer_mac) | — |
| Driver y motor paso a paso | Toshiba TB67S109 + NEMA 23 | `STEP` → GPIO3, `DIR` → GPIO4, `ENABLE` (activo en bajo) → GPIO5 |
| Fin de carrera (homing) | Microswitch con rodillo (NO + C, activo en bajo, pull-up interno) | Señal (`NO`) → GPIO10, Común (`C`) → GND |
| Sensor de luz ambiental | BH1750 (I2C `0x23`, 0 a 65 535 lx) | `SDA` → GPIO6, `SCL` → GPIO7, VCC → 3,3 V, GND → GND |
| LED indicador de estado | LED integrado en placa (activo en bajo) | GPIO8 (encendido mientras el motor está energizado) |

## Lo que envía a AURA

Cada medición es un campo de `values` (contrato §3.1). Un sensor que falla o una posición sin calibrar no aparece.

| Campo | Unidad | Rango válido | Cada cuánto |
|---|---|---|---|
| `posicion_pct` | % | 0 a 100 | `intervalo_s` (por defecto 10 s), y al finalizar cada movimiento |
| `luz_lux` | lx | 0 a 65535 | `intervalo_s` (por defecto 10 s) |
| `estado_motor` | estado | 0 a 2 | `intervalo_s` (`0`: detenido, `1`: moviendo, `2`: calibrando) |
| `modo` | modo | 0 a 2 | `intervalo_s` (`0`: manual, `1`: auto1, `2`: auto2) |

Alertas que emite (contrato §3.5): `sensor` cuando el BH1750 no responde por I2C (`sin_respuesta_i2c`) o cuando falla la calibración inicial con el fin de carrera (`hall_activo_al_inicio`, `calibracion_sin_hall`, `recorrido_muy_corto`).

## Configuración y comandos

Parámetros que acepta `set_config` (contrato §3.3). Se valida todo junto y, si algo está mal, no se aplica nada. Los umbrales, el intervalo y el tiempo de confirmación se guardan en NVS; al reiniciar siempre arranca en modo manual (`0`) por seguridad mecánica.

| Parámetro | Rango | Valor por defecto |
|---|---|---|
| `intervalo_s` | 2 a 3600 | 10 |
| `modo` | `0` (`"manual"`), `1` (`"auto1"`), `2` (`"auto2"`) | 0 |
| `umbral_bajo_lux` | 0 a 100000 (menor que `umbral_alto_lux`) | 200 |
| `umbral_alto_lux` | 0 a 100000 (mayor que `umbral_bajo_lux`) | 1000 |
| `confirmacion_s` | 0 a 600 | 5 |

Comandos que acepta `ejecutar_comando` (solo en modo manual; en modo automático rechaza con `modo_automatico`):
- `cortina`: `{"posicion_pct": 0..100}` o `{"accion": "abrir" | "cerrar"}`.
- `led`: `{"encendido": true | false}` o `{"invertir": true}` (compatibilidad con el botón local del concentrador de aula: alterna entre 0 % y 100 %).

## Compilar y probar

1. Copiar `main/config_local.ejemplo.h` como `main/config_local.h` y completar `MESH_ID_BYTES` y `MESH_AP_PASS`. Para pruebas de banco sin malla, poner `#define AURA_MODO_BANCO 1`.
2. Compilar con `idf.py build`.
3. Firmar la imagen antes de grabar (`Mesh_Red_Completa/GUIA_NODOS_HOJA.md`, §2.9):
   ```powershell
   python -m espsecure sign-data --version 2 --keyfile <ruta>/firma_prueba.pem --output build/firmado.bin build/aura-cortina.bin
   move /y build\firmado.bin build\aura-cortina.bin
   ```
4. Grabar por USB con `idf.py -p COM3 flash monitor`.
