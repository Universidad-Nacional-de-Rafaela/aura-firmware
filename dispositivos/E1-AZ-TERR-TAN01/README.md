# E1-AZ-TERR-TAN01 — nivel de dos tanques de agua de la terraza

| | |
|---|---|
| **Ubicación** | Edificio 1, azotea, terraza (al aire libre) |
| **Responsable** | losHijosDelStackOverFlow, Ingeniería en Computación IV 2026 (@FerChaparro27, @romerof7) |
| **Estado** | en desarrollo: el firmware y el conector los sube el grupo en su PR |
| **Instalado** | todavía no |
| **Transporte** | LoRaWAN (exterior), según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) y [ADR-006](../../docs/adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.0 |
| **Basado en** | ninguno |
| **Códigos anteriores** | ninguno |

## Qué hace

Mide la distancia del sensor a la superficie del agua en cada uno de los dos tanques de la
terraza, con una sola placa (contrato §2.4). El nivel y el volumen los calcula el conector en AURA.

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
