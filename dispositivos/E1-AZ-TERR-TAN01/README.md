# E1-AZ-TERR-TAN01 — nivel de dos tanques de agua de la terraza

| | |
|---|---|
| **Ubicación** | Edificio 1, azotea, terraza (al aire libre) |
| **Responsable** | losHijosDelStackOverFlow, Ingeniería en Computación IV 2026 (@FerChaparro27, @romerof7) |
| **Estado** | instalado |
| **Instalado** | 2026-10-21 |
| **Transporte** | LoRaWAN (exterior), según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) y [ADR-006](../../docs/adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.0 |
| **Basado en** | ninguno |
| **Códigos anteriores** | ninguno |

## Qué hace

Mide la distancia del sensor a la superficie del agua en cada uno de los dos tanques de la
terraza, con una sola placa (contrato §2.4). El nodo manda solo esa distancia; el porcentaje y los
litros los calcula el conector en AURA con las medidas de cada tanque.

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | Seeed XIAO ESP32S3, `hw_id` `eui-<DevEUI>` (lo asigna la cátedra en ChirpStack) | — |
| Radio | Wio-SX1262 (SX1262), AU915 sub-banda 2, OTAA, LoRaWAN 1.0.x, clase C | conector B2B: NSS 41, DIO1 39, NRST 42, BUSY 40, ANT_SW 38 |
| Sensor de nivel, tanque 1 | JSN-SR04T 2.0 (ultrasónico, sumergible, 5 V) | Trig D1, Echo D2 |
| Sensor de nivel, tanque 2 | JSN-SR04T 2.0 (ultrasónico, sumergible, 5 V) | Trig D5, Echo D4 |

| Tanque | Modelo | Alto × diámetro | Capacidad | Distancia del sensor, vacío | Distancia del sensor, lleno |
|---|---|---|---|---|---|
| 1 | Eternit tricapa, vertical | 1,37 × 1,05 m | 1000 L | 137 cm | 40 cm |
| 2 | Rotoplas Slim (boca de 0,49 m) | 1,50 a 1,55 × 0,68 a 0,70 m | 500 L | 152,5 cm | 25 cm |

El sensor está en la tapa: la distancia con el tanque vacío es el alto del tanque (en el 2, el
medio del rango del fabricante). La de lleno es el nivel máximo del agua, medido en la terraza.

Estas medidas no están en el firmware: están en `TANQUES`, en
[`conector.py`](../../conectores/E1-AZ-TERR-TAN01/conector.py). Si cambian, o el nodo se usa en otro
tanque, se cambian ahí y la placa no se toca. El volumen supone sección constante (tanque
cilíndrico o de paredes rectas).

## Lo que envía a AURA

El nodo manda `dist_t1_cm` y `dist_t2_cm` (contrato §3.1); el conector agrega el nivel y el
volumen al guardar. Un sensor que falla no aparece, y de ese tanque no se calcula nada. Los rangos
son los del manifiesto del conector, [`conector.toml`](../../conectores/E1-AZ-TERR-TAN01/conector.toml).

| Campo | Unidad | Rango válido | Cada cuánto |
|---|---|---|---|
| `dist_t1_cm`, `dist_t2_cm` | cm | 0 a 600 | `intervalo_s` (60 s de fábrica), del nodo |
| `nivel_t1_pct`, `nivel_t2_pct` | % | 0 a 100 | con cada medición, calculado por el conector |
| `vol_t1_l` | L | 0 a 1000 | con cada medición, calculado por el conector |
| `vol_t2_l` | L | 0 a 500 | con cada medición, calculado por el conector |

Alertas: el nodo no emite alertas propias. El conector avisa, una vez por cruce y por tanque:

- `nivel_bajo` cuando un tanque baja del 20 % y cuando se recupera.
- `nivel_alto` cuando un tanque llega al 95 % y cuando baja. Una distancia menor que la de lleno
  (40 cm en el tanque 1, 25 cm en el 2) se guarda como 100 %: es el agua por encima de la marca o
  el sensor obstruido por condensación, y el aviso lo dice. Por debajo de 20 cm (la zona ciega del
  JSN-SR04T) el nodo la manda igual.

## Codec

[`codec.js`](codec.js) es el codec del device profile `aura-tanque-2x-ultrasonico` de ChirpStack,
tal como está cargado ahí. Lo mantiene la cátedra: un cambio se hace primero acá y después se
copia al profile. Si cambia el formato de los bytes, va un device profile nuevo (contrato §7).

### Subida

| fPort | Bytes | Qué es |
|---|---|---|
| 1 | 4 | Medición: distancia de cada tanque, uint16 big endian, cm × 10; `0xFFFF` = sin lectura (el campo no aparece en `values`) |
| 2 | 4 | Reporte de un comando: `ref`, resultado (0 aplicado, 1 fuera de rango, 2 comando desconocido), intervalo vigente en s (uint16) |

### Bajada (fPort 10)

| Comando | Bytes | Qué hace |
|---|---|---|
| `medir_ahora` | `01 ref` | mide y envía sin esperar al intervalo |
| `set_config {"intervalo_s": 60..43200}` | `02 ref hi lo` | cambia el intervalo de lectura |

El nodo contesta cada comando con un reporte en el fPort 2 (contrato §3.4), y después de
`medir_ahora` manda además la medición en el fPort 1. El profile es clase C y su *uplink interval*
es 43200 s, el máximo de `set_config`: el `lorawan-bridge` da la placa por offline después de
3 × ese intervalo sin uplinks.

## Comandos

El nodo es **clase C**: escucha todo el tiempo, así que un comando llega en segundos y no espera
al próximo uplink. El `ref` (1 a 255) lo pone el `lorawan-bridge` y el nodo lo devuelve en el
reporte.

| Bajada | Bytes | Qué hace el nodo |
|---|---|---|
| `medir_ahora` | `01 ref` | mide y manda la medición (fPort 1), y manda el reporte (fPort 2) con resultado 0 |
| `set_config` | `02 ref hi lo` | `s = hi<<8 \| lo`. Si 60 ≤ s ≤ 43200: lo guarda en NVS, lo aplica y reporta resultado 0. Si no: no cambia nada y reporta resultado 1 |
| otro primer byte | `?? ref …` | no hace nada y reporta resultado 2 |

El reporte va siempre en el fPort 2, con 4 bytes: `[ref, resultado, intervalo_hi, intervalo_lo]`,
con el intervalo vigente en segundos (después de aplicar, si se aplicó). Resultado: 0 aplicado,
1 fuera de rango, 2 comando desconocido.

- Entre dos uplinks, reportes incluidos, pasan al menos 10 s: lo que llega antes espera su turno.
- Si llega dos veces seguidas el mismo `ref`, el nodo manda de nuevo el reporte sin volver a
  aplicar el comando.
- Un downlink de menos de 2 bytes no trae `ref`: se ignora sin reporte.

## Configuración

Se cambia con `set_config` (contrato §3.3):

| Parámetro | Rango | Default | Dónde se guarda |
|---|---|---|---|
| `intervalo_s` | 60 a 43200 s | 60 s | NVS (`Preferences`, espacio `config`, clave `intervalo_s`) |

Al arrancar, el nodo lee `intervalo_s` de la NVS; si falta o no valida, usa 60 s. El nodo de la
terraza está en 300 s, por un `set_config` desde AURA.

Las credenciales OTAA (DevEUI, JoinEUI, AppKey) no van en este repo: las da la cátedra y van en
`config_local.h`.

## Compilar y probar

1. Abrir `E1-AZ-TERR-TAN01.ino` en el IDE de Arduino, copiar `config_local.h.example` como
   `config_local.h` y completar las credenciales OTAA. Sin ese archivo compila y mide (se ve por el
   monitor serie), pero no se une a la red. Bibliotecas: las de `bibliotecas.txt`.
2. En ChirpStack, con un uplink en *Events* del dispositivo, `object` tiene que traer
   `dist_t1_cm` y `dist_t2_cm`. Con un comando, el reporte del fPort 2 trae `ref`, `aplicado` y
   `config.intervalo_s`.
3. El conector se prueba sin placa: `python3 -m unittest discover -s conectores/E1-AZ-TERR-TAN01/tests`.

## Notas

El primer firmware mandaba 10 bytes, con la distancia, el porcentaje y los litros de cada tanque
calculados en la placa, y el codec los llamaba `distancia_t1`, `porcentaje_t1` y `litros_t1`. Se
pasó a mandar solo la distancia para que las medidas de los tanques se puedan cambiar sin
reflashear. Al cambiar el firmware hay que reemplazar el codec en el device profile de ChirpStack:
el viejo rechaza los uplinks de 4 bytes.
