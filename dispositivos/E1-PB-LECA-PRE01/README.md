# E1-PB-LECA-PRE01 — Presencia de personas por cámara (MCCV)

| | |
|---|---|
| **Ubicación** | Edificio 1, planta baja, LabECA (a confirmar con la cátedra) |
| **Responsable** | Grupo MCCV, Ingeniería en Computación IV 2026: `MatiLang1`, `AndresFranco1` |
| **Estado** | en desarrollo |
| **Instalado** | todavía no |
| **Transporte** | mesh ESP-WIFI-MESH (interior), según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) y [ADR-006](../../docs/adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.4 |
| **Basado en** | [`Mesh_Red_Completa/Mesh3_Hoja`](../../Mesh_Red_Completa/Mesh3_Hoja/) |
| **Códigos anteriores** | ninguno |

## Qué hace

Informa si hay personas en el aula. La detección la hace una Raspberry Pi 4 con cámara (código
en el repo del grupo, `mccv-edge`); la Pi le pasa el estado a esta hoja por UART y la hoja lo
publica por la mesh. Una Pi sin cámara confiable, o desconectada, no genera datos falsos: la
hoja avisa con la alerta `sensor`.

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | ESP32-C3 SuperMini, `hw_id` `mac-…` (herramientas/leer_mac, a completar) | USB a la Pi (alimentación) |
| Detector | Raspberry Pi 4 + cámara, corre `mccv-edge` | UART: Pi GPIO14 TXD (pin 8) → C3 **GPIO4** (RX); Pi GPIO15 RXD (pin 10) ← C3 **GPIO5** (TX); GND (pin 6) ↔ GND |
| LED | LED azul de la SuperMini | GPIO8: encendido = hay personas |

Las dos placas son de 3,3 V: se conectan directo. UART a 115200 8N1.

### Protocolo Pi → hoja

Una línea ASCII terminada en `\n`. La Pi la manda en cada cambio y la repite cada 5 s.

| Línea | Significa |
|---|---|
| `P1 72` / `P0 90` | hay / no hay personas, confianza 72 % / 90 % |
| `P1` / `P0` | igual, sin confianza |
| `U` | la cámara no da una lectura confiable |

Cualquier otra línea se descarta y se loguea (`protocolo_pi.c`, con tests de host en `tests/`).

## Lo que envía a AURA

Cada medición es un campo de `values` (contrato §3.1). Un sensor que falla no aparece.

| Campo | Unidad | Rango válido | Cada cuánto |
|---|---|---|---|
| `presencia` | booleano | `true` / `false` | 10 s (configurable) y enseguida en cada cambio |
| `conf_pct` | % | 0 a 100 | junto con `presencia`, si la Pi la manda |

Alertas que emite (contrato §3.5): `sensor` con `campo: presencia` y `motivo: sin_respuesta`
cuando la Pi manda `U` o pasan 30 s sin una línea válida; `recuperada` cuando vuelve. Una vez
por cambio (`nodo_mesh_sonda`).

## Configuración

Parámetros que acepta `set_config` (contrato §3.3), y el valor que usa esta placa.

| Parámetro | Rango | Valor en esta placa |
|---|---|---|
| `intervalo_s` | 2 a 3600 | 10 |

Cualquier otro parámetro (salvo `aula`, que maneja la biblioteca) se rechaza con
`parametro_desconocido`; `intervalo_s` tiene que ser un entero (si no, `intervalo_s_fuera_de_rango`).
El valor se guarda en NVS antes de aplicarse; si no se puede guardar, responde `error_de_nvs` y no
cambia nada. No acepta comandos propios.

## Compilar y probar

1. ESP-IDF 6.1. Copiar `main/config_local.ejemplo.h` como `main/config_local.h` y completar el
   ID y la clave de la mesh (los da la cátedra; no se sube).
2. `idf.py set-target esp32c3` y `idf.py -p <puerto> build flash monitor`.
3. Tests de host del protocolo: `make -C tests`.
4. Sin la Pi: conectar un adaptador USB-serie a GPIO4/GND y escribir `P1 72` + Enter. En el
   monitor se ve `Pi: P1 72` y, en el broker de banco, `hw/<hw_id>/data` con
   `{"presencia":true,"conf_pct":72}`. Dejar de escribir 30 s → alerta `sensor`.

## Notas

- El firmware es el mismo para cualquier aula: el aula la asigna AURA (`set_config {"aula": …}`).
- Tipo de firmware: `aura-presencia`, versión en `CMakeLists.txt`.
