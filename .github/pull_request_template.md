## Qué cambia

<!-- Una o dos oraciones. Si es un dispositivo nuevo, cuál y para qué. -->

## Dispositivo

- Carpeta: `dispositivos/<nombre>/`
- Transporte: mesh ESP-NOW / LoRaWAN
- Contrato que implementa: v3.0

## Checklist

- [ ] No hay credenciales, claves ni `config_local.h` en el PR (el repo es público).
- [ ] El README del dispositivo está completo: hardware, campos de `values`, alertas,
      parámetros de `set_config`.
- [ ] `bibliotecas.txt` lista todas las bibliotecas, con versión.
- [ ] Si toqué `comun/` o un sketch con `#include` de `comun/`, corrí
      `herramientas/generar_autocontenidos.sh` y commiteé `autocontenido/`.
- [ ] Lo probé en hardware: <qué se probó y qué se vio>.
