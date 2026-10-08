## Qué cambia

<!-- Una o dos oraciones. Si es un dispositivo nuevo, cuál y para qué. -->

## Dispositivo

- Carpeta: `dispositivos/<CÓDIGO>/` (nuevo / modificado)
- Transporte: mesh ESP-WIFI-MESH / LoRaWAN
- Contrato que implementa: v3.0

## Checklist

- [ ] No hay credenciales, claves ni `config_local.h` en el PR (el repo es público).
- [ ] La ficha (`README.md`) está completa: ubicación, hardware, campos de `values`, alertas,
      parámetros de `set_config`.
- [ ] El dispositivo figura en la tabla de `dispositivos/README.md`, y las abreviaturas nuevas
      están en el mapa.
- [ ] `bibliotecas.txt` lista todas las bibliotecas, con versión.
- [ ] Si toqué `comun/` o un sketch con `#include` de `comun/`, corrí
      `herramientas/generar_autocontenidos.sh` y commiteé `autocontenido/`.
- [ ] Lo probé en hardware: <qué se probó y qué se vio>.
