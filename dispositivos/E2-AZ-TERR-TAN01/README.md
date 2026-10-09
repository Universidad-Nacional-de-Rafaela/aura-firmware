# E2-AZ-TERR-TAN01 — nivel de tres tanques de agua de la terraza

Medición del nivel de agua de tres tanques, con una sola placa y tres sensores ultrasónicos.

| | |
|---|---|
| **Ubicación** | Edificio 2, azotea (terraza), sobre los tres tanques de agua |
| **Responsable** | Grupo de IC IV 2026 (`@forzanijuan22`, `@AlejoRacca`, `@Agucesano`) |
| **Estado** | en desarrollo |
| **Instalado** | todavía no |
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
| Placa | Seeed XIAO ESP32S3, `hw_id` `eui-<DevEUI>` (el DevEUI del nodo) | — |
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

Los tres tanques se miden en turnos de 20 s: el 1 en el segundo 0, el 2 en el 20 y el 3 en el 40 de cada minuto (el segundo 0 es el arranque de la placa). El uplink sale después de medir el tanque 3.

El firmware manda la distancia de cada tanque como la mediana de 5 lecturas. Si el sensor no
responde, o la distancia está fuera de rango, manda un valor especial (`0xFFFF` sin eco, `0xFFFE`
demasiado cerca, `0xFFFD` demasiado lejos). Ese valor **no es una medición**: el codec ([`codec.js`](codec.js)) no lo pone en `values`. El
contador de lectura y el campo reservado del uplink tampoco van en `values`.

Alertas que emite (contrato §3.5):
- `sensor`: el nodo LoRaWAN no publica alertas propias. La genera el conector cuando falta el campo
  de un tanque en un mensaje que sí trae otro (el sensor dejó de responder o dio un valor fuera de
  rango). El `lorawan-bridge` (`aura-app`, `servicios/lorawan_bridge/`) publica en
  `hw/eui-<DevEUI>/data` lo que devuelve el codec.
- `tanque_bajo`, desde el conector: `warning` cuando el agua se aleja más del umbral y `info`
  cuando vuelve.

## Configuración

Parámetros que acepta `set_config` (contrato §3.3), y el valor que usa esta placa. Llegan por
downlink de 2 bytes (segundos, big-endian). Solo valores **no secretos**.

| Parámetro | Rango | Valor en esta placa |
|---|---|---|
| `intervalo_s` | 60 a 65535 (se redondea a múltiplos de 60) | 60 (se guarda en memoria no volátil) |

## Codec

[`codec.js`](codec.js) es el codec del device profile `aura-clase-a` en ChirpStack: hay que pegarlo
ahí cada vez que cambie. Devuelve solo `distancia_tanqueN_mm` y omite el campo de un sensor que
falla. Tiene que coincidir con los campos de [`conector.toml`](../../conectores/E2-AZ-TERR-TAN01/conector.toml).
El downlink de `set_config` (`intervalo_s`) lo codifica en 2 bytes big-endian.

## Compilar y probar

1. Copiá `credenciales.h.example` como `credenciales.h` (en esta carpeta) y completá `JOIN_EUI`,
   `DEV_EUI` y `APP_KEY`, que da la cátedra. `credenciales.h` está en el `.gitignore`: **no se
   versiona**.
2. Bibliotecas: las de `bibliotecas.txt` (`RadioLib`); `Preferences` viene con el core de ESP32.
3. En banco: el monitor serie muestra cada lectura, el intento de envío y si el servidor confirmó.

## Notas

- El firmware es `E2-AZ-TERR-TAN01.ino`, en esta carpeta, y parte de `nodo_lorawan.ino` de
  `IC-lorawan-test`.
- Pendiente: el reporte bajo demanda (downlink de 1 byte con la máscara de tanques, respuesta por
  el puerto 2) todavía no tiene codec: `codec.js` solo decodifica el puerto 1.
- Pendiente: el firmware todavía no informa `aplicado` por uplink después de un `set_config`
  (contrato §3.4).
