#!/usr/bin/env python3
"""Valida las carpetas de dispositivos/ contra el mapa de dispositivos/README.md.

Cada carpeta (salvo las que empiezan con _):
- se llama con un código EDIFICIO-PISO-RECINTO-TIPOnn cuyas partes están en las tablas
  del mapa, numerado desde 01;
- figura en la tabla "Dispositivos" del mapa;
- tiene su ficha README.md;
- si tiene firmware, es un solo .ino llamado igual que la carpeta (Arduino lo exige).

Uso: herramientas/validar_dispositivos.py   (desde cualquier carpeta; sale con 1 si hay errores)
"""
import re
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
DISPOSITIVOS = RAIZ / "dispositivos"
CODIGO = re.compile(r"^([A-Z0-9]+)-([A-Z0-9]+)-([A-Z0-9]+)-([A-Z]{3})(\d{2})$")


def secciones(texto):
    """Devuelve {titulo: [códigos de la primera columna]} para cada sección ## o ###."""
    resultado, actual = {}, None
    for linea in texto.splitlines():
        m = re.match(r"^#{2,3} (.+)$", linea)
        if m:
            actual = m.group(1).strip()
            resultado.setdefault(actual, [])
            continue
        celda = re.match(r"^\|\s*\[?`([^`]+)`", linea)
        if actual and celda:
            resultado[actual].append(celda.group(1))
    return resultado


mapa = secciones((DISPOSITIVOS / "README.md").read_text(encoding="utf-8"))
partes = {
    "edificio": set(mapa.get("Edificios", [])),
    "piso": set(mapa.get("Pisos", [])),
    "recinto": set(mapa.get("Recintos", [])),
    "tipo": set(mapa.get("Tipos", [])),
}
listados = set(mapa.get("Dispositivos", []))
errores = []

for carpeta in sorted(p for p in DISPOSITIVOS.iterdir() if p.is_dir()):
    nombre = carpeta.name
    if nombre.startswith("_"):
        continue
    m = CODIGO.match(nombre)
    if not m:
        errores.append(f"dispositivos/{nombre}: el código tiene que ser EDIFICIO-PISO-RECINTO-TIPOnn")
        continue
    for clave, valor in zip(("edificio", "piso", "recinto", "tipo"), m.groups()[:4]):
        if valor not in partes[clave]:
            errores.append(f"dispositivos/{nombre}: {clave} '{valor}' no está en el mapa de dispositivos/README.md")
    if m.group(5) == "00":
        errores.append(f"dispositivos/{nombre}: la numeración empieza en 01")
    if nombre not in listados:
        errores.append(f"dispositivos/{nombre}: falta en la tabla 'Dispositivos' de dispositivos/README.md")
    if not (carpeta / "README.md").is_file():
        errores.append(f"dispositivos/{nombre}: falta la ficha README.md")
    for ino in sorted(carpeta.glob("*.ino")):
        if ino.name != f"{nombre}.ino":
            errores.append(f"dispositivos/{nombre}: el firmware tiene que llamarse {nombre}.ino, no {ino.name}")

for codigo in sorted(listados - {p.name for p in DISPOSITIVOS.iterdir() if p.is_dir()}):
    errores.append(f"dispositivos/README.md: {codigo} está en la tabla pero no tiene carpeta")

for e in errores:
    print(f"::error::{e}")
print(f"{len(errores)} errores" if errores else "dispositivos/ en orden")
sys.exit(1 if errores else 0)
