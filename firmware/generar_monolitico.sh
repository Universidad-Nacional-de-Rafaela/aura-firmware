#!/bin/bash
# Genera versiones de un solo archivo de cada sketch, con los headers de
# comun/ incrustados. Sirven para pegar directo en el IDE de Arduino sin
# depender de la estructura de carpetas.
#
# NO editar lo que hay en monolitico/: se regenera. La fuente de verdad son
# los .ino de cada nodo y los headers de comun/.
#
# Uso: ./generar_monolitico.sh   (desde firmware/)

set -e
cd "$(dirname "$0")"
mkdir -p monolitico

inlinear() {
  local entrada="$1" salida="$2"
  : > "$salida"
  while IFS= read -r linea; do
    if [[ "$linea" =~ ^#include\ \"\.\./comun/(.+)\"$ ]]; then
      local h="comun/${BASH_REMATCH[1]}"
      echo "// ---------- inicio de $h ----------" >> "$salida"
      grep -v '^#pragma once' "$h" | grep -v '^#include "protocolo_aura.h"' >> "$salida"
      echo "// ---------- fin de $h ----------" >> "$salida"
    else
      echo "$linea" >> "$salida"
    fi
  done < "$entrada"
}

for nodo in nodo_sensor nodo_sala nodo_gateway; do
  if [ -f "$nodo/$nodo.ino" ]; then
    mkdir -p "monolitico/$nodo"
    inlinear "$nodo/$nodo.ino" "monolitico/$nodo/$nodo.ino"
    echo "generado monolitico/$nodo/$nodo.ino ($(wc -l < "monolitico/$nodo/$nodo.ino") lineas)"
  fi
done
