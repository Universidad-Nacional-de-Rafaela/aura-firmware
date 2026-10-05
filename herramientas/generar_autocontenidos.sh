#!/bin/bash
# Genera una copia autocontenida de cada sketch en autocontenido/<nombre>/: la carpeta
# del sketch más los headers de comun/ que usa, copiados como pestañas. Sirve para
# abrir el sketch en el IDE de Arduino sin depender de la estructura del repo.
#
# Los headers se copian como archivos aparte y NO se incrustan en el .ino: el IDE
# genera prototipos de las funciones del .ino y los pone antes de los struct
# incrustados, y el sketch deja de compilar ('BufferCircular' was not declared).
#
# NO editar autocontenido/ a mano: se regenera. La fuente de verdad son las
# carpetas de infraestructura/, firmware/ y ejemplos/, y los headers de comun/.
#
# Uso: herramientas/generar_autocontenidos.sh   (desde cualquier carpeta)

set -euo pipefail
raiz="$(cd "$(dirname "$0")/.." && pwd)"
cd "$raiz"
rm -rf autocontenido
mkdir -p autocontenido

for sketch in infraestructura/*/ firmware/*/ ejemplos/*/; do
  nombre="$(basename "$sketch")"
  [ -f "$sketch/$nombre.ino" ] || continue
  destino="autocontenido/$nombre"
  mkdir -p "$destino"

  # La carpeta del sketch, salvo su documentación y lo que no compila Arduino.
  find "$sketch" -maxdepth 1 -type f \
    ! -name README.md ! -name bibliotecas.txt ! -name 'codec*.js' ! -name config_local.h \
    -exec cp {} "$destino/" \;

  # Los headers de comun/ que incluye, y los que esos headers incluyen a su vez.
  for h in $(grep -ho '#include "\(\.\./\)*comun/[^"]*"' "$sketch"/*.ino "$sketch"/*.h 2>/dev/null \
             | sed 's|.*comun/\([^"]*\)"|\1|' | sort -u); do
    cp "comun/$h" "$destino/"
    for dep in $(grep -ho '#include "[^"/]*\.h"' "comun/$h" | sed 's|#include "\(.*\)"|\1|'); do
      [ -f "comun/$dep" ] && cp "comun/$dep" "$destino/"
    done
  done

  # Dentro de la copia, los headers están al lado del .ino.
  sed -i 's|#include "\(\.\./\)*comun/\([^"]*\)"|#include "\2"|' "$destino"/*.ino "$destino"/*.h 2>/dev/null || true
  sed -i "1i // GENERADO por herramientas/generar_autocontenidos.sh: no editar. Fuente: $sketch" "$destino/$nombre.ino"
  echo "generado $destino ($(ls "$destino" | wc -l) archivos)"
done
