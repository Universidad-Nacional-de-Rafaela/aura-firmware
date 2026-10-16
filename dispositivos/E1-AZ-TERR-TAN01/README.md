# E1-AZ-TERR-TAN01 — nivel de dos tanques de agua de la terraza

| | |
|---|---|
| **Ubicación** | Edificio 1, azotea, terraza (al aire libre) |
| **Responsable** | losHijosDelStackOverFlow, Ingeniería en Computación IV 2026 (@FerChaparro27, @romerof7) |
| **Estado** | en desarrollo: llega a ChirpStack; el firmware se sube a esta carpeta |
| **Instalado** | todavía no |
| **Transporte** | LoRaWAN (exterior), según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) y [ADR-006](../../docs/adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.0 |
| **Basado en** | ninguno |
| **Códigos anteriores** | ninguno |

## Qué hace

Mide el nivel de agua de dos tanques de la terraza con una sola placa (contrato §2.4): la
distancia del sensor a la superficie de cada tanque. El nodo manda solo esa distancia; el porcentaje
y los litros los calcula el conector en AURA con las medidas de cada tanque.

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | Seeed XIAO ESP32S3, `hw_id` `eui-<DevEUI>` (lo asigna la cátedra en ChirpStack) | — |
| Radio | Wio-SX1262 (SX1262), AU915 sub-banda 2, OTAA, LoRaWAN 1.0.x | conector B2B: NSS 41, DIO1 39, NRST 42, BUSY 40, ANT_SW 38 |
| Sensor de nivel, tanque 1 | JSN-SR04T 2.0 (ultrasónico, sumergible, 5 V) | Trig D1, Echo D2 |
| Sensor de nivel, tanque 2 | JSN-SR04T 2.0 (ultrasónico, sumergible, 5 V) | Trig D5, Echo D4 |

| Tanque | Capacidad | Distancia del sensor, vacío | Distancia del sensor, lleno |
|---|---|---|---|
| 1 | 1000 L | 170 cm | 20 cm |
| 2 | 1500 L | 200 cm | 20 cm |

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
| `dist_t1_cm`, `dist_t2_cm` | cm | 0 a 600 | 1 min, del nodo |
| `nivel_t1_pct`, `nivel_t2_pct` | % | 0 a 100 | 1 min, calculado por el conector |
| `vol_t1_l` | L | 0 a 1000 | 1 min, calculado por el conector |
| `vol_t2_l` | L | 0 a 1500 | 1 min, calculado por el conector |

Alertas: el nodo no emite alertas propias. El conector avisa, una vez por cruce y por tanque:

- `nivel_bajo` cuando un tanque baja del 20 % y cuando se recupera.
- `nivel_alto` cuando un tanque llega al 95 % y cuando baja. Una distancia menor a 20 cm (la zona
  ciega del JSN-SR04T, que coincide con la marca de lleno) se manda igual y se guarda como 100 %:
  es el agua por encima de la marca o el sensor obstruido por condensación, y el aviso lo dice.

### Payload y codec

El nodo manda 4 bytes por uplink, que decodifica [`codec.js`](codec.js) en el device profile:

| Bytes | Campo | Codificación |
|---|---|---|
| 0-1 | `dist_t1_cm` | uint16 big endian, cm × 10 |
| 2-3 | `dist_t2_cm` | uint16 big endian, cm × 10 |

Una distancia que el firmware no pudo medir se manda como `0xFFFF`, y el codec la omite de
`values`. Cambiar el formato de los bytes requiere un device profile nuevo, no editar
este codec (contrato §7).

## Comandos

El nodo es **clase C** (contrato §3.3): escucha todo el tiempo, así que un comando llega en
segundos y no espera al próximo uplink.

| Comando | Downlink | Qué hace |
|---|---|---|
| `{"command": "medir_ahora"}` | 1 byte, `0x01` (fPort 10) | mide los dos tanques y manda el uplink de siempre, sin esperar al intervalo |

El byte lo arma `encodeDownlink` de [`codec.js`](codec.js); un comando que no conoce sale con
error y no se encola. El firmware ignora cualquier otro downlink. Entre dos envíos pasan al menos
10 s: un `medir_ahora` que llega antes queda pendiente hasta entonces. No hay un uplink de
reporte con el `command_id` (contrato §3.4): la confirmación es la medición nueva.

Para que ChirpStack mande downlinks en clase C, el device profile tiene que tener habilitado
*Supports Class-C*. Hasta que exista el `lorawan-bridge`, el comando se prueba encolándolo a mano
en la pestaña *Queue* del dispositivo (fPort 10, como JSON `{"command": "medir_ahora"}` o como
bytes `01`).

## Configuración

Todavía no acepta `set_config` (contrato §3.3): no tiene parámetros remotos. Las credenciales
OTAA (DevEUI, JoinEUI, AppKey) no van en este repo: las da la cátedra y van en `config_local.h`.

## Compilar y probar

1. Abrir `E1-AZ-TERR-TAN01.ino` en el IDE de Arduino, copiar `config_local.h.example` como
   `config_local.h` y completar las credenciales OTAA. Sin ese archivo compila y mide (se ve por el
   monitor serie), pero no se une a la red. Bibliotecas: las de `bibliotecas.txt`.
2. En ChirpStack, el device profile usa `codec.js` como *JavaScript functions*. Con un uplink,
   en *Events* del dispositivo, `object` tiene que traer `dist_t1_cm` y `dist_t2_cm`.
3. El conector se prueba sin placa: `python3 -m unittest discover -s conectores/E1-AZ-TERR-TAN01/tests`.

## Notas

El primer firmware mandaba 10 bytes, con la distancia, el porcentaje y los litros de cada tanque
calculados en la placa, y el codec los llamaba `distancia_t1`, `porcentaje_t1` y `litros_t1`. Se
pasó a mandar solo la distancia para que las medidas de los tanques se puedan cambiar sin
reflashear. Al cambiar el firmware hay que reemplazar el codec en el device profile de ChirpStack:
el viejo rechaza los uplinks de 4 bytes.
