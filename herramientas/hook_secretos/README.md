# Revisión de secretos en tu PC

Una clave subida a GitHub queda publicada para siempre: borrar el archivo no alcanza, porque
sigue en el historial, y la única salida es cambiar la clave. Esta herramienta revisa, **antes
de que salga de tu PC**, cada commit y cada push de **todos** tus repos, no solo los de AURA.

Frena el commit o el push si encuentra:

- **archivos que nunca se suben:** `config_local.h`, `credenciales.h`, `.env`, `*.pem`, `*.key`
  y cualquier cosa dentro de una carpeta `claves/`;
- **algo con forma de clave en el código:** `#define WIFI_PASS "…"`, `MQTT_PASS`,
  `MESH_AP_PASS`, `MESH_ID_BYTES`, claves AES de LoRaWAN, `password = …`, claves privadas.

Los valores de ejemplo pasan: `"CAMBIAR"`, `""`, `"<clave del WiFi>"`, bytes en cero. Por eso
las plantillas `config_local.ejemplo.h` se pueden subir.

## Instalar (una vez por PC)

Necesitás git y Python 3.8 o más nuevo (el de ESP-IDF sirve). Desde una copia de este repo:

**Windows** (PowerShell):

```powershell
powershell -ExecutionPolicy Bypass -File herramientas\hook_secretos\instalar.ps1
```

**Linux o macOS:**

```sh
sh herramientas/hook_secretos/instalar.sh
```

Para actualizar, se vuelve a correr lo mismo. Para quitarlo: `instalar.ps1 -Desinstalar` o
`instalar.sh --desinstalar`.

Para probarlo, en un repo cualquiera: creá un `config_local.h`, hacé `git add` y `git commit`.
Tiene que frenarlo.

## Si te frena

El mensaje dice qué archivo y qué línea, sin mostrar la clave. Sacalo del commit
(`git restore --staged <archivo>`), agregá el archivo al `.gitignore` o dejá un valor de
ejemplo, y volvé a hacer el commit. Si esa clave **ya estaba subida**, avisale a la cátedra:
hay que cambiarla.

El push también se revisa, porque `git revert`, `cherry-pick` y `merge` no pasan por la revisión
del commit, y pueden volver a poner una clave que ya habías sacado.

Si creés que es un falso positivo, consultá a la cátedra. `git commit --no-verify` saltea la
revisión, pero entonces la responsabilidad es tuya.

## Lo que no cubre

- Lo que se edita, se sube o se revierte **desde la web de GitHub**: no pasa por tu PC.
- Un repo que define su propio `core.hooksPath` (por ejemplo, con husky): git usa el de ese
  repo y no esta revisión.
- Los otros hooks del repo (`commit-msg`, etc.) dejan de correr mientras esté instalada; el
  `pre-commit` y el `pre-push` propios del repo sí corren, después de la revisión.
