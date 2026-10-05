## Qué cambia

<!-- Una o dos oraciones. Si es un firmware o un dispositivo nuevo, cuál y para qué. -->

## Firmware y dispositivos

- Firmware: `firmware/Fnnn_descripcion/` (nuevo / modificado)
- Dispositivos: `dispositivos/<CÓDIGO>/`
- Transporte: mesh ESP-NOW / LoRaWAN
- Contrato que implementa: v3.0

## Checklist

- [ ] No hay credenciales, claves ni `config_local.h` en el PR (el repo es público).
- [ ] El README del firmware está completo: hardware, campos de `values`, alertas,
      parámetros de `set_config`.
- [ ] Cada dispositivo tiene su ficha, figura en la tabla de `dispositivos/README.md`, y las
      abreviaturas nuevas están en el mapa.
- [ ] `bibliotecas.txt` lista todas las bibliotecas, con versión.
- [ ] Si toqué `comun/` o un sketch con `#include` de `comun/`, corrí
      `herramientas/generar_autocontenidos.sh` y commiteé `autocontenido/`.
- [ ] Lo probé en hardware: <qué se probó y qué se vio>.
