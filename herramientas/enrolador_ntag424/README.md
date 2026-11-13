# Estación de enrolamiento NTAG 424 DNA

Es lo único del sistema que **cambia la clave de una tarjeta**. Las puertas solo autentican. Se
conecta por USB a una PC y no se une a la mesh.

Cada tarjeta lleva su propia clave AES-128: `clave = HMAC-SHA256(maestra, UID)[0..16]`. Una tarjeta
enrolada con una maestra solo la abren las puertas que tengan **la misma** maestra.

## Hardware

XIAO ESP32C3 + RC522 por SPI, con los mismos pines que la puerta: SCK 6, MISO 10, MOSI 20, SS 21,
RST 7. Conviene un capacitor de 100 µF en la alimentación del RC522: sin él se reinicia solo con
los picos de la antena (la estación lo detecta y lo levanta, pero cuesta un reintento).

## Cargar el firmware

Sketch Arduino (`enrolador_ntag424.ino`), core `esp32:esp32`, placa `XIAO_ESP32C3` con las opciones
por defecto. Las bibliotecas están en `bibliotecas.txt`.

## Usarla (`enrolar.py`, solo biblioteca estándar)

```bash
python3 herramientas/enrolador_ntag424/enrolar.py info
python3 herramientas/enrolador_ntag424/enrolar.py uid          # UID de la tarjeta apoyada
python3 herramientas/enrolador_ntag424/enrolar.py estado       # qué clave tiene (no cambia nada)
python3 herramientas/enrolador_ntag424/enrolar.py enrolar      # de fábrica -> clave derivada
python3 herramientas/enrolador_ntag424/enrolar.py desenrolar   # clave derivada -> de fábrica
python3 herramientas/enrolador_ntag424/enrolar.py lote         # una tras otra, Ctrl+C para cortar
```

`enrolar` y `desenrolar` leen el UID, lo muestran y piden confirmación (`--si` la saltea). Después
le piden a la estación que actúe **solo sobre esa tarjeta**: si hay otra en el campo, no se le toca.
Si hay más de un puerto serie, indicá el de la estación con `--puerto`.

Resultados de `estado`: `enrolada`, `de_fabrica`, o algo que requiere mirar la tarjeta a mano
(`incierta`, `posiblemente_perdida`).

## Modo autónomo (botón y buzzer, sin PC)

Con un botón en el GPIO 4 (entre el pin y GND) y un buzzer pasivo en el GPIO 3, la estación enrola sola:

- **Botón suelto:** apoyar una tarjeta la **enrola**. Dos tonos que suben.
- **Botón apretado desde antes** (0,4 s como mínimo): apoyar una tarjeta la **borra**, la devuelve a la clave de fábrica. Dos tonos que bajan.
- **Ya estaba así:** un tono corto, no se tocó nada.
- **No se pudo:** un tono grave largo; la tarjeta sigue como estaba.
- **Peligro** (`incierta` o `posiblemente_perdida`): grave y agudo alternados; mirar esa tarjeta antes de seguir.

Una tarjeta ya atendida no se vuelve a atender hasta que se la retire (se la mira cada 150 ms y hace falta
~0,5 s de ausencia). Cada una imprime una línea `AUTO <acción> <UID> <resultado>` por el serie. Cualquier orden por
USB suspende el modo autónomo 30 s para no pisar a `enrolar.py`; `enrolar.py auto on|off` lo controla.

## Clave maestra

- **Modo banco** (`ENROLADOR_MAESTRA_EFUSE 0`): la maestra se guarda en la flash **sin protección**.
  Sirve para probar y **no** para tarjetas de verdad. Se carga con `enrolar.py maestra ARCHIVO`
  (64 caracteres hexa en un archivo; nunca por la línea de comandos, queda en el historial).
- **Producción** (`ENROLADOR_MAESTRA_EFUSE 1`): la clave vive en un eFuse y la placa calcula el HMAC
  sin que salga del chip. **No se puede leer, cambiar ni deshacer**, así que se graba una sola vez,
  en la estación y en cada cerradura, con la misma clave. Hay que decidir el respaldo antes.

### Pasar una placa a eFuse (irreversible)

1. Si la placa tuvo antes una maestra de banco, **borrá la NVS entera**:
   `esptool --chip esp32c3 erase-region 0x9000 0x5000`. Marcarla como borrada desde el firmware no alcanza:
   los bytes de la clave siguen en la flash y anulan la protección.
2. Verificá que parte de cero: `espefuse --chip esp32c3 summary` (bloques de clave libres, Secure Boot apagado).
3. Quemá la clave de 32 bytes (archivo binario) con propósito `HMAC_UP`:
   `espefuse --chip esp32c3 burn-key BLOCK_KEY0 clave.bin HMAC_UP`. Por defecto queda protegida contra
   lectura y escritura. Pide escribir `BURN`.
4. Antes de usarla con tarjetas, comprobá que el chip calcula lo mismo que el software
   (`esp_hmac_calculate` contra `HMAC-SHA256` de la PC, para unos cuantos UID).
5. Compilá con `ENROLADOR_MAESTRA_EFUSE 1`. En ese modo la estación borra la copia de banco y rechaza `maestra`.

La clave de prueba que se usó en banco es conocida: no sirve para tarjetas de verdad.

## Qué puede salir mal

- **Perder la clave 0 deja la etiqueta inservible** (no hay reinicio de fábrica sin ella). Por eso
  la estación siempre comprueba con la clave nueva después de cambiarla y no reintenta a ciegas lo
  que pudo haber quedado a medias.
- No activar LRP en la etiqueta: es irreversible.
- Con varias tarjetas cerca la anticolisión falla: probar de a una.

## Tests

La lógica (`enrolador_logica.h`) no toca hardware y se prueba en la compu:
`make -C herramientas/enrolador_ntag424/tests`.
