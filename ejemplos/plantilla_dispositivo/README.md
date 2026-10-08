# <CÓDIGO> — <qué es, en palabras>

> Plantilla: copiar esta carpeta como `dispositivos/<CÓDIGO>/`, renombrar el `.ino` igual que
> la carpeta, reemplazar lo que está entre `< >` y borrar esta línea. El código se arma con el
> mapa de [`dispositivos/README.md`](../../dispositivos/README.md).

| | |
|---|---|
| **Ubicación** | <Edificio 1, planta baja, LabECA — dentro del tablero / junto a la ventana / …> |
| **Responsable** | <grupo, materia y año, con los usuarios de GitHub; o la cátedra> |
| **Estado** | <en desarrollo / en banco / instalado / fuera de servicio> |
| **Instalado** | <AAAA-MM-DD, o "todavía no"> |
| **Transporte** | <mesh ESP-WIFI-MESH (interior) / LoRaWAN (exterior)>, según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) y [ADR-006](../../docs/adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v<4.0> |
| **Basado en** | <ninguno — o el código del dispositivo del que se copió el firmware> |
| **Códigos anteriores** | <ninguno — o el código que tenía antes de mudarse> |

## Qué hace

<Una o dos oraciones: qué mide o qué acciona, y sobre qué equipo o espacio.>

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | Seeed XIAO ESP32S3, `hw_id` `<mac-…>` (herramientas/leer_mac) | — |
| <sensor> | <modelo> | <GPIO> |

## Lo que envía a AURA

Cada medición es un campo de `values` (contrato §3.1). Un sensor que falla no aparece.

| Campo | Unidad | Rango válido | Cada cuánto |
|---|---|---|---|
| `<temp_c>` | °C | <-55 a 125> | <60 s, configurable> |

Alertas que emite (contrato §3.5): <`sensor` cuando …>.

## Configuración

Parámetros que acepta `set_config` (contrato §3.3), y el valor que usa esta placa. Solo valores
**no secretos**: los secretos van en `config_local.h`, que no se versiona.

| Parámetro | Rango | Valor en esta placa |
|---|---|---|
| `<intervalo_s>` | <5 a 86400> | <60> |

## Compilar y probar

1. Copiar `config_local.h.example` como `config_local.h` y completarlo.
2. Bibliotecas: las de `bibliotecas.txt`.
3. <Cómo se prueba en banco y qué se espera ver en el monitor serie.>

## Notas

<Lo que necesita saber quien venga después: cómo se accede, qué se probó, qué falló.>
