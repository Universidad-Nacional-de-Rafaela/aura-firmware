## Qué cambia

<!-- Una o dos oraciones. Si es un dispositivo nuevo, cuál y para qué. -->

## Dispositivo

- Carpeta: `dispositivos/<CÓDIGO>/` (nuevo / modificado)
- Conector: `conectores/<CÓDIGO>/` (nuevo / modificado / sin cambios)
- Transporte: mesh ESP-WIFI-MESH / LoRaWAN
- Tipo y versión de firmware (mesh): `aura-<uso>` `x.y.z`
- Contrato que implementa: v<la de `docs/CONTRATO_MQTT.md`>

## Checklist

- [ ] No hay credenciales, claves de firma, `config_local.h`, `credenciales.h` ni `.bin` en el PR (el repo es público).
- [ ] No toca `comun/`, `infraestructura/` ni el código de la cátedra en `Mesh_Red_Completa/` (eso va en un PR aparte).
- [ ] La ficha (`README.md`) está completa: ubicación, hardware, campos de `values`, alertas,
      parámetros de `set_config`.
- [ ] El dispositivo figura en la tabla de `dispositivos/README.md`, las abreviaturas nuevas
      están en el mapa, y la carpeta está en `.github/CODEOWNERS`.
- [ ] Mesh: `project(aura-<uso>)` y `PROJECT_VER` subido respecto de la entrega anterior.
- [ ] LoRaWAN (Arduino): `bibliotecas.txt` lista todas las bibliotecas, con versión.
- [ ] El conector declara en `conector.toml` cada campo que manda el firmware, y sus tests pasan.
- [ ] Probado en hardware. Mesh: las 9 pruebas obligatorias de `Mesh_Red_Completa/GUIA_HOJAS_NUEVAS.md`
      (paso 5) en el banco del grupo, con el informe en `dispositivos/<CÓDIGO>/pruebas/`.
      LoRaWAN: <qué se probó y qué se vio>.
