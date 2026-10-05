# Dispositivos instalados

Una carpeta por **placa instalada**, con su ficha (`README.md`): dónde está, qué firmware lleva,
quién es responsable. El código de la carpeta dice **dónde está y qué es**, de un vistazo.

El código de un dispositivo **no es su firmware**: los cinco dispositivos de un laboratorio
pueden usar cinco firmwares distintos, y un mismo firmware puede estar en muchos lugares. El
firmware está en [`../firmware/`](../firmware/), y cada ficha dice cuál usa.

## Formato del código

```
EDIFICIO-PISO-RECINTO-TIPOnn          ejemplo: E1-PB-LECA-HFR01
```

Mayúsculas, separado por guiones. `nn` numera los dispositivos del mismo tipo en el mismo
recinto, desde `01`. Cada parte sale de las tablas de abajo: **si necesitás una abreviatura
nueva, agregala a su tabla en el mismo PR** que crea el dispositivo. El CI rechaza códigos que
no respeten el formato o que usen abreviaturas que no están acá.

**Si un equipo se muda**, su carpeta se renombra con el código nuevo y la ficha anota el
anterior en *Códigos anteriores*. En AURA conserva su `device_id`, así que el historial de
mediciones no se corta.

## Mapa

Las abreviaturas siguen el modelo de ubicaciones de AURA (edificio → piso → recinto).

### Edificios

| Código | Edificio |
|---|---|
| `E1` | Edificio 1 |

### Pisos

| Código | Piso | Número en AURA |
|---|---|---|
| `PB` | Planta baja | 0 |
| `P1` | Primer piso | 1 |
| `AZ` | Azotea | 2 |

### Recintos

| Código | Recinto | Edificio y piso |
|---|---|---|
| `LECA` | LabECA | E1, planta baja |
| `TERR` | Terraza (al aire libre) | E1, azotea |

### Tipos

El tipo dice **sobre qué actúa** el dispositivo: la magnitud que mide en el ambiente, el equipo
que monitorea, o lo que acciona. Una placa que mide varias magnitudes del ambiente a la vez es
`AMB`, y su ficha lista qué mide.

| Código | Qué es | Clase |
|---|---|---|
| `TEM` | Temperatura del ambiente | magnitud |
| `HUM` | Humedad del ambiente | magnitud |
| `LUZ` | Intensidad de luz | magnitud |
| `AMB` | Varias magnitudes del ambiente en una placa | magnitud |
| `HFR` | Heladera con freezer | equipo |
| `HEL` | Heladera | equipo |
| `FRZ` | Freezer | equipo |
| `INC` | Incubadora | equipo |
| `HOR` | Horno | equipo |
| `TAB` | Tablero eléctrico (consumo) | equipo |
| `CRT` | Cortina | actuador |
| `GWM` | Gateway de la mesh | infraestructura |
| `SAL` | Nodo de sala de la mesh | infraestructura |

## Dispositivos

| Código | Qué es | Firmware | Estado |
|---|---|---|---|
| [`E1-PB-LECA-HFR01`](E1-PB-LECA-HFR01/) | Heladera-freezer del LabECA | `F001` | en desarrollo |
