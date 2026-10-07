#!/usr/bin/env python3
"""Valida los conectores: los de conectores/ y los de ejemplos/ (*conector*).

Cada carpeta de conectores/ (salvo las que empiezan con _):
- se llama con el código de un dispositivo (EDIFICIO-PISO-RECINTO-TIPOnn), como su carpeta
  en dispositivos/;
- carga con aura_sdk: manifiesto válido, nombre igual a la carpeta, clase que hereda de
  Conector e implementa al_recibir_datos;
- tiene README.md y tests/ con al menos un test_*.py.

Uso: herramientas/validar_conectores.py   (sale con 1 si hay errores)
"""
import re
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RAIZ / "comun" / "python"))
from aura_sdk import ManifiestoInvalido, cargar_conector  # noqa: E402

CODIGO = re.compile(r"^([A-Z0-9]+)-([A-Z0-9]+)-([A-Z0-9]+)-([A-Z]{3})(\d{2})$")


def validar(carpeta, exigir_codigo):
    errores = []
    if exigir_codigo and not CODIGO.match(carpeta.name):
        errores.append("el nombre tiene que ser el código del dispositivo (E1-PB-LECA-HFR01)")
    try:
        cargar_conector(carpeta)
    except ManifiestoInvalido as e:
        errores.append(str(e))
    if not (carpeta / "README.md").is_file():
        errores.append("falta README.md")
    if not list((carpeta / "tests").glob("test_*.py")):
        errores.append("falta tests/test_*.py")
    return errores


def main():
    carpetas = [(d, True) for d in sorted((RAIZ / "conectores").glob("*/")) if not d.name.startswith("_")]
    carpetas += [(d, False) for d in sorted((RAIZ / "ejemplos").glob("*conector*/"))]
    total = 0
    for carpeta, exigir in carpetas:
        errores = validar(carpeta, exigir)
        total += len(errores)
        for e in errores:
            print(f"{carpeta.relative_to(RAIZ)}: {e}")
    print(f"conectores: {len(carpetas)} revisados, {total} errores" if total else
          f"conectores en orden ({len(carpetas)})")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
