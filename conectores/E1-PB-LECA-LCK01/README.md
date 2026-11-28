# E1-PB-LECA-LCK01 — conector del gabinete de lockers del pañol

Procesa lo que manda la hoja del gabinete de lockers del pañol del LabECA (ESP32-C3 SuperMini,
hoja de la mesh): una placa, 4 lockers. Para cada locker, el estado de la puerta y de la cerradura.

## Campos

Tienen que coincidir con lo que manda el firmware (`values`, contrato v4.7, propuesto en el PR del
dispositivo). Son estados: solo se guardan 0 y 1.

| Campos | Unidad | Rango | |
|---|---|---|---|
| `puerta_1` a `puerta_4` | 0/1 | 0 o 1 | 1 = abierta (microswitch libre), 0 = cerrada |
| `cerradura_1` a `cerradura_4` | 0/1 | 0 o 1 | 1 = energizada (la puerta se puede abrir), 0 = trabada |

Un valor intermedio (por ejemplo 0.5) está dentro del rango pero no es un estado: el conector lo
quita. Un campo de un locker que no existe (`puerta_5`) no está declarado y AURA no lo guarda. Si
en una muestra no queda ningún estado válido, la descarta (`sin_estados_validos`).

## Alertas

El conector no emite alertas propias: `intrusion`, `arranque_puerta_abierta`, `puerta_abierta`,
`puerta_cerrada` y `limite_pulsos` las emite la placa, con los tipos del contrato v4.5 (§3.5) y
el número de locker en `details.puerta`. Ver la ficha en
[`dispositivos/E1-PB-LECA-LCK01/`](../../dispositivos/E1-PB-LECA-LCK01/).

## Probarlo

Desde la raíz del repo:

```
python -m unittest discover -s conectores\E1-PB-LECA-LCK01\tests
python herramientas\correr_conector.py conectores\E1-PB-LECA-LCK01 --archivo conectores\E1-PB-LECA-LCK01\tests\mensajes_ejemplo.jsonl --subproceso
```

`tests/mensajes_ejemplo.jsonl` es una apertura del locker 2 armada a mano con el formato de
`data` del gateway (cerrado, pulso, abierto con el pulso ya cortado, cerrado); se reemplaza por
mensajes reales del banco cuando estén las pruebas.
