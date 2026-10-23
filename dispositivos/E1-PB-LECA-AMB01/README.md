# E1-PB-LECA-AMB01 — CO2 del LabECA

Medición de CO2 del aire del laboratorio, con un sensor MH-Z19C y tres LEDs locales que indican el nivel.

| | |
|---|---|
| **Ubicación** | Edificio 1, planta baja, LabECA |
| **Responsable** | Grupo de IC IV 2026 (@AgosBracaccini, <completar con los usuarios del grupo>) |
| **Estado** | en banco: el firmware compila y mide con el sensor conectado a la placa |
| **Instalado** | todavía no |
| **Transporte** | mesh ESP-WIFI-MESH (interior), según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) y [ADR-006](../../docs/adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.0 |
| **Basado en** | `Mesh_Red_Completa/Mesh3_Hoja` |
| **Códigos anteriores** | ninguno |

## Qué hace

Mide la concentración de CO2 del aire cada 10 segundos y la manda a AURA. Los LEDs de la placa muestran el nivel en el lugar: verde, amarillo o rojo, según dos umbrales configurables.

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | ESP32-C3 SuperMini, `hw_id` `mac-907069abbfe4` (herramientas/leer_mac) | — |
| Sensor de CO2 | MH-Z19C (NDIR, 0 a 5000 ppm), UART a 9600 baudios | sensor TX (verde) → GPIO3, sensor RX (azul) → GPIO4, Vin (rojo) → 5 V, GND (negro) → GND |
| LED verde | LED + resistencia de 220 ohm | GPIO5 |
| LED amarillo | LED + resistencia de 220 ohm | GPIO6 |
| LED rojo | LED + resistencia de 220 ohm | GPIO7 |

## Lo que envía a AURA

Cada medición es un campo de `values` (contrato §3.1). Un sensor que falla no aparece.

| Campo | Unidad | Rango válido | Cada cuánto |
|---|---|---|---|
| `co2_ppm` | ppm | 0 a 5000 | 10 s, configurable |

Durante los primeros 3 minutos desde el arranque el sensor se calienta: no se publica nada y el LED amarillo parpadea.

Alertas que emite (contrato §3.5): `sensor` cuando el MH-Z19C no responde (`sin_respuesta`) o da un valor fuera de rango (`fuera_de_rango`). Las alertas por nivel de CO2 las calcula el conector dentro de AURA; los LEDs de la placa son independientes de eso.

## Configuración

Parámetros que acepta `set_config` (contrato §3.3), y el valor que usa esta placa. Se valida todo junto y, si algo está mal, no se aplica nada. Los valores se guardan en la memoria de la placa. El parámetro `aula` lo asigna AURA y la placa lo acepta.

| Parámetro | Rango | Valor en esta placa |
|---|---|---|
| `intervalo_s` | 5 a 3600 | 10 |
| `atencion_ppm` | 400 a 5000 | 1000 |
| `alto_ppm` | 400 a 5000 | 1500 |

`atencion_ppm` tiene que ser menor que `alto_ppm`. LED verde hasta `atencion_ppm`, amarillo hasta `alto_ppm`, rojo por encima.

## Compilar y probar

1. Copiar `main/config_local.ejemplo.h` como `main/config_local.h` y completar el ID y la clave de la mesh (los da la cátedra). Ese archivo no se versiona.
2. Dependencias: no usa `bibliotecas.txt` (es ESP-IDF). La única externa, `cjson`, se baja sola desde `main/idf_component.yml`.
3. `idf.py set-target esp32c3` y `idf.py -p <puerto> build flash monitor`.
4. En el banco se espera: 3 minutos con el LED amarillo parpadeando y después una línea `CO2: <n> ppm` cada 10 segundos en el monitor serie, con el LED que corresponda encendido.

## Notas

- La calibración automática (ABC) del sensor está apagada, porque en un aula no hay 24 h seguidas de aire exterior. Pendiente: calibrar el cero a 400 ppm al aire libre.
- Usar un cable USB de datos para grabar la placa; un cable solo de carga no la hace aparecer como puerto.
- La temperatura interna del MH-Z19C no se publica: no es una temperatura ambiente confiable.