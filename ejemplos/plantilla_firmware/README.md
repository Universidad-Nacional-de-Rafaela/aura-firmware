# Fnnn — <descripción corta>

> Plantilla: reemplazar todo lo que está entre `< >` y borrar esta línea.

| | |
|---|---|
| **Autores** | <grupo, con sus usuarios de GitHub> |
| **Materia y año** | <IC III / IC IV, 2026> |
| **Estado** | <en desarrollo / probado en banco / en uso> |
| **Transporte** | <mesh ESP-NOW (interior) / LoRaWAN (exterior)>, según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v<3.0> |

## Qué hace

<Una o dos oraciones: qué mide o qué acciona. Dónde está instalado no va acá: lo dicen las
fichas de `dispositivos/` que usan este firmware.>

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | Seeed XIAO ESP32S3 | — |
| <sensor> | <modelo> | <GPIO> |

Variantes que acepta con opciones de `config_local.h` (por ejemplo, otra pantalla):
<ninguna / `#define PANTALLA_SSD1306` …>.

## Lo que envía a AURA

Cada medición es un campo de `values` (contrato §3.1). Un sensor que falla no aparece.

| Campo | Unidad | Rango válido | Cada cuánto |
|---|---|---|---|
| `<temp_c>` | °C | <-55 a 125> | <60 s, configurable> |

Alertas que emite (contrato §3.5): <`sensor` cuando …>.

## Configuración remota

Parámetros que acepta `set_config` (contrato §3.3):

| Parámetro | Rango | Por defecto |
|---|---|---|
| `<intervalo_s>` | <5 a 86400> | <60> |

## Compilar y probar

1. Copiar `config_local.h.example` como `config_local.h` y completarlo.
2. Bibliotecas: las de `bibliotecas.txt`.
3. <Cómo se prueba en banco y qué se espera ver en el monitor serie.>
