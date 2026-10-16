# E2-AZ-TERR-TAN01 — nivel de tres tanques de agua de la terraza

Medición del nivel de agua de tres tanques, con una sola placa y tres sensores ultrasónicos.

| | |
|---|---|
| **Ubicación** | Edificio 2, azotea (terraza), sobre los tres tanques de agua |
| **Responsable** | Grupo de IC IV 2026 (`@forzanijuan22`, `@AlejoRacca`, `@Agucesano`) |
| **Estado** | en desarrollo |
| **Instalado** | todavía no |
| **Transporte** | LoRaWAN (exterior), clase A, según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.0 |
| **Basado en** | ninguno (el firmware parte de `nodo_lorawan.ino` de `IC-lorawan-test`; los puertos y los comandos siguen el codec de [`E1-AZ-TERR-TAN01`](../E1-AZ-TERR-TAN01/)) |
| **Códigos anteriores** | ninguno |

## Qué hace

Mide la distancia del sensor a la superficie del agua en cada uno de los tres tanques y la manda
por LoRaWAN. Más agua significa menos distancia. El conector
([`conectores/E2-AZ-TERR-TAN01/`](../../conectores/E2-AZ-TERR-TAN01/)) guarda las mediciones y avisa
cuando un tanque queda bajo o un sensor deja de medir.

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | Seeed XIAO ESP32S3, `hw_id` `eui-<DevEUI>` (el DevEUI del nodo) | — |
| Radio | Wio-SX1262, región AU915, subbanda 2 | pines propios del módulo |
| Sensor del tanque 1 | AJ-SR04M | TRIG `D0`, ECHO `D1` |
| Sensor del tanque 2 | AJ-SR04M | TRIG `D2`, ECHO `D3` |
| Sensor del tanque 3 | AJ-SR04M | TRIG `D4`, ECHO `D5` |

## Lo que envía a AURA

Cada medición es un campo de `values` (contrato §3.1). Un sensor que falla no aparece, y si no
mide ninguno, no se publica `data`.

| Campo | Unidad | Rango válido | Cada cuánto |
|---|---|---|---|
| `distancia_tanque1_mm` | mm | 250 a 6000 | 60 s, configurable; y al pedir `medir_ahora` |
| `distancia_tanque2_mm` | mm | 250 a 6000 | ídem |
| `distancia_tanque3_mm` | mm | 250 a 6000 | ídem |

Los tres tanques se miden en turnos de 20 s: el 1 en el segundo 0, el 2 en el 20 y el 3 en el 40
de cada minuto (el segundo 0 es el arranque de la placa). La medición periódica sale después de
medir el tanque 3, con la última medición de cada tanque, que vive solo en RAM. Ningún comando
mueve los turnos: la red y `medir_ahora` usan los huecos entre turnos.

La distancia de cada tanque es la mediana de 5 lecturas. Si el sensor no responde o la distancia
está fuera de rango, el firmware manda un valor especial (`0xFFFF` sin eco, `0xFFFE` demasiado
cerca, `0xFFFD` demasiado lejos). Ese valor **no es una medición**: el codec no lo pone en `values`.

Alertas (contrato §3.5). El nodo LoRaWAN no publica alertas propias: las genera el conector.
- `sensor`: cuando falta el campo de un tanque en un mensaje que sí trae otro, e `info` cuando vuelve.
- `tanque_bajo`: `warning` cuando el agua se aleja más del umbral e `info` cuando vuelve.

## Comandos

Llegan por el `lorawan-bridge` (contrato §3.3), por downlink en el fPort 10. El nodo contesta
cada uno con un reporte en el fPort 2 que lleva el `ref` del comando, el resultado y el
`intervalo_s` vigente: el bridge lo publica como `aplicado` o `rechazado` (§3.4).

| Comando | Bytes | Qué hace |
|---|---|---|
| `medir_ahora` | `01 ref` | mide los tres tanques en el próximo hueco entre turnos (sin mover el ciclo) y manda la medición enseguida del reporte |
| `set_config {"intervalo_s": 60..43200}` | `02 ref hi lo` | cambia cada cuánto sale la medición periódica |

| Parámetro de `set_config` | Rango | Valor en esta placa |
|---|---|---|
| `intervalo_s` | 60 a 43200; se redondea hacia arriba a un múltiplo de 60 y el reporte informa el que quedó | 60 (se guarda en memoria no volátil) |

Un valor fuera de rango no cambia nada y se contesta con `fuera_de_rango`. Un comando que el
nodo no conoce, o mal armado, se contesta con `comando_desconocido`.

**Clase A**: el nodo solo escucha después de transmitir, así que un comando llega con el próximo
uplink, hasta `intervalo_s` después de encolarlo (60 s por defecto, hasta 12 h con el máximo).

## Codec

[`codec.js`](codec.js) es el codec del device profile de este nodo en ChirpStack. Un cambio se
hace primero acá y después se copia al profile. Si cambia el formato de los bytes, va un device
profile nuevo (contrato §7). Los tests están en [`tests/`](tests/) (`make -C tests`, con Node).

### Subida

| fPort | Bytes | Qué es |
|---|---|---|
| 1 | 6 | Medición: distancia de cada tanque en mm, uint16 big endian. `0xFFFF`, `0xFFFE`, `0xFFFD` o fuera de 250..6000: el campo no aparece en `values` |
| 2 | 4 | Reporte de un comando: `ref`, resultado (0 aplicado, 1 fuera de rango, 2 comando desconocido), intervalo vigente en s (uint16) |

### Device profile

| Campo | Valor |
|---|---|
| Codec | el contenido de [`codec.js`](codec.js) |
| Región, versión | AU915 subbanda 2, LoRaWAN 1.0.x, OTAA, clase A |
| *Uplink interval* | 43200 s, el máximo de `set_config` (el bridge infiere `offline` a 3 × ese valor) |
| Tag `aura_max_downlink_bytes` | `4` (el comando más largo) |

## Compilar y probar

1. Copiá `config_local.h.example` como `config_local.h` (en esta carpeta) y completá `JOIN_EUI`,
   `DEV_EUI` y `APP_KEY`, que da la cátedra. `config_local.h` está en el `.gitignore`: **no se
   versiona**. Sin ese archivo el sketch compila, pero no se une a la red.
2. Placa: `esp32:esp32:XIAO_ESP32S3` (`fqbn.txt`), core ESP32 3.3.11. Bibliotecas: las de
   `bibliotecas.txt` (`RadioLib`); `Preferences` viene con el core.
3. En banco: el monitor serie muestra cada lectura, cada envío, si el servidor confirmó y cada
   comando que llega.

## Notas

- Pendiente: probar en la placa y en ChirpStack.
- Pendiente: `UMBRAL_BAJO_MM` del conector es provisional, hasta medir la altura de los tanques.
