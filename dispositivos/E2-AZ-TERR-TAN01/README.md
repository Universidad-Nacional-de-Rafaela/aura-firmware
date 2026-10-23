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

Cada medición es un campo de `values` (contrato §3.1).

| Campo | Unidad | Rango válido | Cuándo |
|---|---|---|---|
| `distancia_tanqueN_cm` (N = 1, 2, 3) | cm, con un decimal | 25 a 600 | cada `intervalo_s` (60 s por defecto), los tres juntos; y al pedir `medir_ahora` |
| `nivel_tanqueN_pct` | %, con un decimal | 0 a 100 | junto con la distancia, solo si el tanque está calibrado |

**Mide los tres y manda.** Cada `intervalo_s` se miden los tres tanques, uno detrás del otro
(menos de 2 s), y enseguida sale un mensaje con los tres. Los ciclos se cuentan desde el primero,
sin deriva.

La distancia de cada tanque es la mediana de 5 lecturas. Si el sensor no responde o mide más
lejos que el rango, ese tanque **no va en el mensaje**: es una falla, y el conector avisa `sensor`
cuando el tanque deja de llegar mientras los otros siguen llegando.

**Tanque lleno.** El sensor va en la tapa y el AJ-SR04M no mide a menos de unos 25 cm (zona
ciega). Con el tanque lleno el agua puede quedar dentro de esa zona: el firmware manda `0xFFFE` y
el codec lo informa como **25 cm**, el nivel máximo que el sensor puede medir. No es una falla
sino la saturación del rango, como un transmisor 4-20 mA en su tope: `25` quiere decir "25 cm
o menos". No dispara la alerta `sensor` ni `tanque_bajo`.

**Nivel.** Con el cero y el lleno de un tanque (ver *Comandos*), el nodo calcula
`nivel = (cero − distancia) / (cero − lleno)`, recortado a 0..100 %. Sin calibrar, solo manda la
distancia.

Alertas (contrato §3.5). El nodo LoRaWAN no publica alertas propias: las genera el conector.
- `sensor`: un tanque falta en 3 mensajes seguidos que traen a los otros (3 ciclos, sea cual sea
  el intervalo); `info` cuando vuelve.
- `tanque_bajo`: `warning` cuando el nivel baja del 20 % e `info` cuando vuelve a 25 % o más.
  Necesita el tanque calibrado. La bomba la maneja un control externo: el aviso sirve para saber
  que no llenó a tiempo.

## Comandos

Llegan por el `lorawan-bridge` (contrato §3.3), por downlink en el fPort 10. El nodo contesta
cada uno con un reporte en el fPort 2 que lleva el `ref` del comando, el resultado y la
configuración vigente: el bridge lo publica como `aplicado` o `rechazado` (§3.4).

| Comando | Bytes | Qué hace |
|---|---|---|
| `medir_ahora` | `01 ref` | mide los tres tanques ya (sin mover el ciclo) y los manda, enseguida del reporte |
| `set_config` solo con `intervalo_s` | `02 ref hi lo` | cambia el intervalo (4 bytes: entra con `aura_max_downlink_bytes = 4`) |
| `set_config` con calibración | `02 ref` + una entrada `id hi lo` por parámetro | cambia los parámetros de abajo |

| Parámetro de `set_config` | Rango | Qué es | Valor en esta placa |
|---|---|---|---|
| `intervalo_s` | 60 a 43200 s, entero | cada cuánto se miden los tres tanques y se manda; el ciclo arranca de nuevo con el intervalo nuevo | 60 |
| `cero_tanqueN_cm` | 25 a 600 cm, un decimal | distancia del sensor al agua con el tanque **vacío** (0 %) | sin calibrar |
| `lleno_tanqueN_cm` | 25 a 600 cm, un decimal | distancia del sensor al agua con el tanque **lleno** (100 %) | sin calibrar |

Todo se guarda en memoria no volátil. El cero tiene que quedar al menos 10 cm más lejos que el
lleno. Se valida todo el comando antes de aplicar: si algo no valida, no cambia nada y se contesta
con el motivo (`fuera_de_rango`, `calibracion_invalida` o `comando_desconocido`).

Para calibrar un tanque: con el tanque vacío (o en el nivel que se quiera tomar como 0 %),
`medir_ahora` y anotar `distancia_tanqueN_cm`; lo mismo lleno. Después, por ejemplo,
`set_config {"cero_tanque1_cm": 160, "lleno_tanque1_cm": 30}`.

**Clase A**: el nodo solo escucha después de transmitir, así que un comando llega con el próximo
mensaje: hasta `intervalo_s` después de encolarlo (1 min con 60 s; 12 h con el máximo).

## La red

- **Join**: si falla, reintenta con espera creciente (30 s, 1, 2, 4 y 5 min, con algo de azar) y
  sigue midiendo mientras tanto.
- **Se recupera sola**: si la red deja de confirmar 12 uplinks seguidos (por ejemplo, la cátedra
  reseteó la sesión o los nonces, borró y volvió a crear el device o le cambió el device
  profile), el nodo descarta su sesión y hace un join nuevo. Los DevNonce están en memoria no
  volátil y no se repiten: no hace falta resetearlos en ChirpStack después de reflashear (mientras
  no se borre la flash entera).

## Codec

[`codec.js`](codec.js) es el codec del device profile `aura-tanque-3x-ultrasonico` de ChirpStack
(antes `aura-clase-a`). Un cambio se
hace primero acá y después se copia al profile. Si cambia el formato de los bytes, cambian el
firmware y el codec a la vez. Los tests están en [`tests/`](tests/) (`make -C tests`, con Node).

### Subida

| fPort | Bytes | Qué es |
|---|---|---|
| 1 | 13 con los 3 tanques (9 o 5 si falta alguno) | Medición: máscara de tanques (bit 0 = tanque 1) y, por cada uno, distancia en mm (cm × 10) y nivel en décimas de % (`0xFFFF` = sin calibrar), uint16 big endian. Distancia `0xFFFE` (zona ciega, tanque lleno) = 25 cm |
| 2 | 16 | Reporte de un comando: `ref`, resultado (0 aplicado, 1 fuera de rango, 2 comando desconocido, 3 calibración inválida), `intervalo_s`, cero y lleno de cada tanque en mm (`0xFFFF` = sin calibrar) |

### Bajada (fPort 10)

| Comando | Bytes |
|---|---|
| `medir_ahora` | `01 ref` |
| `set_config` solo con `intervalo_s` | `02 ref hi lo` (s) |
| `set_config` con calibración | `02 ref`, y por parámetro: `01 hi lo` (`intervalo_s`, s), `1N hi lo` (cero del tanque N, mm), `2N hi lo` (lleno del tanque N, mm) |

### Device profile

| Campo | Valor |
|---|---|
| Nombre | `aura-tanque-3x-ultrasonico` |
| Codec | el contenido de [`codec.js`](codec.js) |
| Región, versión | AU915 subbanda 2, LoRaWAN 1.0.x, OTAA, clase A |
| *Uplink interval* | 43200 s, el máximo de `set_config` (el bridge infiere `offline` a 3 × ese valor) |
| Tag `aura_max_downlink_bytes` | `23` (un `set_config` con los 7 parámetros). Con `4` anda el intervalo y `medir_ahora`, pero no la calibración |

## Compilar y probar

1. Copiá `config_local.h.example` como `config_local.h` (en esta carpeta) y completá `JOIN_EUI`,
   `DEV_EUI` y `APP_KEY`, que da la cátedra. `config_local.h` está en el `.gitignore`: **no se
   versiona**. Sin ese archivo el sketch compila, pero no se une a la red.
2. Placa: `esp32:esp32:XIAO_ESP32S3` (`fqbn.txt`), core ESP32 3.3.11. Bibliotecas: las de
   `bibliotecas.txt` (`RadioLib`); `Preferences` viene con el core.
3. En banco: el monitor serie muestra cada lectura (con el nivel, si está calibrado), cada envío,
   si el servidor confirmó y cada comando que llega.
4. **No borrar la flash** al cargar el firmware (en el IDE, *Erase All Flash Before Sketch Upload*
   en *Disabled*): ahí están los DevNonce y la calibración.

## Notas

- Pendiente: probar en la placa y en ChirpStack.
- Pendiente: calibrar el cero y el lleno de cada tanque desde AURA, una vez instalados.
