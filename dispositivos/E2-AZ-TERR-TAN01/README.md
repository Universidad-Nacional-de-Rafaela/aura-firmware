# E2-AZ-TERR-TAN01 — nivel de tres tanques de agua de la terraza

Medición del nivel de agua de tres tanques, con una sola placa y tres sensores ultrasónicos.

| | |
|---|---|
| **Ubicación** | Edificio 2, azotea (terraza), sobre los tres tanques de agua |
| **Responsable** | TODO: grupo, materia y año, con los usuarios de GitHub (`@forzanijuan22`, …) |
| **Estado** | en desarrollo |
| **Instalado** | TODO: AAAA-MM-DD, o "todavía no" |
| **Transporte** | LoRaWAN (exterior), según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.0 |
| **Basado en** | ninguno (el firmware parte de `nodo_lorawan.ino` de `IC-lorawan-test`) |
| **Códigos anteriores** | ninguno |

## Qué hace

Mide la distancia del sensor a la superficie del agua en cada uno de los tres tanques y la manda
por LoRaWAN. Más agua significa menos distancia. El conector
([`conectores/E2-AZ-TERR-TAN01/`](../../conectores/E2-AZ-TERR-TAN01/)) guarda las mediciones y avisa
cuando un tanque queda bajo.

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | Seeed XIAO ESP32S3, `hw_id` `eui-19e2db6c14a8178c` (DevEUI del nodo) | — |
| Radio | Wio-SX1262, región AU915, subbanda 2 | pines propios del módulo |
| Sensor del tanque 1 | AJ-SR04M | TRIG `D0`, ECHO `D1` |
| Sensor del tanque 2 | AJ-SR04M | TRIG `D2`, ECHO `D3` |
| Sensor del tanque 3 | AJ-SR04M | TRIG `D4`, ECHO `D5` |

## Lo que envía a AURA

Cada medición es un campo de `values` (contrato §3.1). Un sensor que falla no aparece.

| Campo | Unidad | Rango válido | Cada cuánto |
|---|---|---|---|
| `distancia_tanque1_mm` | mm | 250 a 6000 | 60 s, configurable |
| `distancia_tanque2_mm` | mm | 250 a 6000 | 60 s, configurable |
| `distancia_tanque3_mm` | mm | 250 a 6000 | 60 s, configurable |

El firmware manda la distancia de cada tanque como la mediana de 5 lecturas. Si el sensor no
responde, o la distancia está fuera de rango, manda un valor especial (`0xFFFF` sin eco, `0xFFFE`
demasiado cerca, `0xFFFD` demasiado lejos). Ese valor **no es una medición**: el codec del device
profile no tiene que ponerlo en `values`. El contador de lectura y el campo reservado del uplink
tampoco van en `values`.

Alertas que emite (contrato §3.5):
- `sensor`, cuando un sensor deja de responder o da un valor fuera de rango. TODO: confirmar con
  la cátedra cómo se informa en LoRaWAN, porque el `lorawan-bridge` todavía no existe.
- `tanque_bajo`, desde el conector: `warning` cuando el agua se aleja más del umbral y `info`
  cuando vuelve.

## Configuración

Parámetros que acepta `set_config` (contrato §3.3), y el valor que usa esta placa. Llegan por
downlink de 2 bytes (segundos, big-endian). Solo valores **no secretos**.

| Parámetro | Rango | Valor en esta placa |
|---|---|---|
| `intervalo_s` | 15 a 65535 | 60 (se guarda en memoria no volátil) |

## Compilar y probar

1. Las credenciales OTAA (`JOIN_EUI`, `DEV_EUI`, `APP_KEY`) las da la cátedra y van en un archivo
   que **no se versiona**.
2. Bibliotecas: `RadioLib` y `Preferences` (incluida en el core de ESP32).
3. En banco: el monitor serie muestra cada lectura, el intento de envío y si el servidor confirmó.

## Notas

- TODO: confirmar con la cátedra dónde se versiona el firmware LoRaWAN, porque la plantilla de
  `dispositivos/` está armada para la mesh.
- Pendiente: el firmware todavía no informa `aplicado` por uplink después de un `set_config`
  (contrato §3.4).
