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
| `distancia_tanque1_cm` | cm, con un decimal | 25 a 600 | cada `intervalo_s` (60 s por defecto); y al pedir `medir_ahora` |
| `distancia_tanque2_cm` | cm, con un decimal | 25 a 600 | ídem |
| `distancia_tanque3_cm` | cm, con un decimal | 25 a 600 | ídem |

Cada `intervalo_s` se miden los tres tanques, en turnos repartidos cada `intervalo_s / 3`: con
60 s, el 1 en el segundo 0, el 2 en el 20 y el 3 en el 40 (el segundo 0 es el arranque de la
placa); con 300 s, cada 100 s. La medición sale después del tanque 3, con la última medición de
cada tanque, que vive solo en RAM. La red y `medir_ahora` usan los huecos entre turnos.

La distancia de cada tanque es la mediana de 5 lecturas. Si el sensor no responde o mide más
lejos que el rango, el firmware manda un valor especial (`0xFFFF` sin eco, `0xFFFD` demasiado
lejos). Eso **es una falla**: el codec no pone el campo en `values` y el conector avisa `sensor`.

**Tanque lleno.** El sensor va en la tapa y el AJ-SR04M no mide a menos de unos 25 cm (zona
ciega). Con el tanque lleno el agua puede quedar dentro de esa zona: el firmware manda `0xFFFE` y
el codec lo informa como **25 cm**, el nivel máximo que el sensor puede medir. No es una falla
sino la saturación del rango, como un transmisor 4-20 mA en su tope: `25` quiere decir "25 cm
o menos". No dispara la alerta `sensor` ni `tanque_bajo`.

Alertas (contrato §3.5). El nodo LoRaWAN no publica alertas propias: las genera el conector.
- `sensor`: cuando falta el campo de un tanque en un mensaje que sí trae otro, e `info` cuando vuelve.
- `tanque_bajo`: `warning` cuando a un tanque le queda menos del 20 % de agua e `info` cuando se
  recupera (con 5 cm de margen). La bomba la maneja un control externo: el aviso sirve para
  saber que no llenó a tiempo.

## Comandos

Llegan por el `lorawan-bridge` (contrato §3.3), por downlink en el fPort 10. El nodo contesta
cada uno con un reporte en el fPort 2 que lleva el `ref` del comando, el resultado y el
`intervalo_s` vigente: el bridge lo publica como `aplicado` o `rechazado` (§3.4).

| Comando | Bytes | Qué hace |
|---|---|---|
| `medir_ahora` | `01 ref` | mide los tres tanques en el próximo hueco entre turnos (sin mover el ciclo) y manda la medición enseguida del reporte |
| `set_config {"intervalo_s": 60..43200}` | `02 ref hi lo` | cambia cada cuánto se miden los tres tanques y se envía; los turnos se recalculan solos (`intervalo_s / 3`) y el ciclo arranca de nuevo por el tanque 1 |

| Parámetro de `set_config` | Rango | Valor en esta placa |
|---|---|---|
| `intervalo_s` | 60 a 43200 s, entero | 60 (se guarda en memoria no volátil) |

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
| 1 | 6 | Medición: distancia de cada tanque en cm × 10 (= mm), uint16 big endian; `values` la trae en cm con un decimal. `0xFFFE` (zona ciega, tanque lleno) se informa como 25 cm. `0xFFFF`, `0xFFFD` o fuera de 25..600 cm: el campo no aparece en `values` |
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
- Pendiente: `ALTURA_CM` del conector tiene valores de prueba (163 cm por tanque), hasta medir la
  distancia del sensor al fondo de cada tanque.
