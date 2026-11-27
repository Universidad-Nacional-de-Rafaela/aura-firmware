# Pruebas de banco de hardware — E1-PB-LECA-CRT01 (2026-11-26)

Registro de las pruebas físicas realizadas sobre la placa **ESP32-C3 SuperMini** (`AURA_MODO_BANCO 1`) con el hardware conectado en banco.

## Alcance y limitaciones
- En este banco se probó únicamente la placa hoja (**ESP32-C3 SuperMini**) con sus periféricos reales.
- **Pendiente:** las 9 pruebas obligatorias de malla, concentrador, OTA y cola (`Mesh_Red_Completa/GUIA_HOJAS_NUEVAS.md`, paso 5) quedan pendientes por no contar en el banco domiciliario con placas ESP32-S3 para levantar el Gateway y el Concentrador (Issue #103).

## Hardware probado
- Driver **Toshiba TB67S109** (`1/8` micropaso) + motor **NEMA 23** (`STEP` GPIO3, `DIR` GPIO4, `ENABLE` GPIO5).
- Sensor de luz ambiental **BH1750** por I2C (`0x23`, `SDA` GPIO6, `SCL` GPIO7).
- Fin de carrera por contacto seco (`GPIO10` con pull-up interno y `GND`), probado con microswitch de rodillo equivalente al reed switch magnético definitivo.

## Casos verificados y extractos de consola serie

### 1. Bus I2C y lectura del BH1750
```text
> i2c
  responde 0x23  (BH1750)
1 dispositivo(s) en el bus (SDA=GPIO6, SCL=GPIO7)

> lux 4
I luz: BH1750 configurado en 0x23
lux = 13.3
lux = 13.3
lux = 13.3
```

### 2. Fin de carrera (GPIO 10)
```text
> hall 10
  t=0.0s  hall=1  libre
  t=5.0s  hall=0  ACTIVADO
  t=5.5s  hall=1  libre
```

### 3. Motor NEMA 23 + homing
- Giro ida y vuelta en `1/8` micropaso (`1600` pasos a `500 us` por semipaso en `DIR=1` y `DIR=0`): movimiento suave sin pérdida de pasos.
- Homing hasta activar el fin de carrera: `estado=detenida calibrada=1 pos=0% pasos=0/2653`.

### 4. Modo automático `auto1` en vivo con BH1750
Con `tot 1600`, `umbrales 50 300 2` (`bajo=50 lx`, `alto=300 lx`, `confirmacion=2 s`) y luz ambiente de `2.5 lx` (posición `0 %`), al iluminar el sensor BH1750:
```text
est: estado=detenida calibrada=1 pos=0% pasos=0/1600 objetivo=0% hall=1 modo=auto1 luz=2.5lx(ok=1)
est: estado=detenida calibrada=1 pos=0% pasos=0/1600 objetivo=0% hall=1 modo=auto1 luz=491.7lx(ok=1)
I luflex: Auto1: 1125.8 lx -> 100%
I cortina: Yendo al 100% (pasos 0 -> 1600)
est: estado=moviendo calibrada=1 pos=46% pasos=749/1600 objetivo=100% hall=1 modo=auto1 luz=1125.8lx(ok=1)
I cortina: Movimiento finalizado: 100%
```
