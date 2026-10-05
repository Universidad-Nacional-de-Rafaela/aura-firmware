#!/usr/bin/env python3
"""Valida los nombres de dispositivos/ y firmware/ contra el mapa y el registro.

- Cada carpeta de dispositivos/ (salvo las que empiezan con _) tiene un código
  EDIFICIO-PISO-RECINTO-TIPOnn cuyas partes están en las tablas del mapa
  (dispositivos/README.md), una ficha README.md, y figura en la tabla "Dispositivos".
- La ficha nombra un firmware Fnnn registrado en firmware/README.md.
- Cada carpeta de firmware/ se llama Fnnn_descripcion, tiene su .ino con el mismo
  nombre, y su número está en el registro, sin repetirse.

Uso: herramientas/validar_dispositivos.py   (desde cualquier carpeta; sale con 1 si hay errores)
"""
import re
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
MAPA = RAIZ / "dispositivos" / "README.md"
REGISTRO = RAIZ / "firmware" / "README.md"
CODIGO = re.compile(r"^([A-Z0-9]+)-([A-Z0-9]+)-([A-Z0-9]+)-([A-Z]{3})(\d{2})$")
FIRMWARE = re.compile(r"^F(\d{3})_[a-z0-9_]+$")

errores = []


def secciones(texto):
    """Devuelve {titulo: [códigos de la primera columna]} para cada sección ### o ##."""
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


mapa = secciones(MAPA.read_text(encoding="utf-8"))
partes = {
    "edificio": set(mapa.get("Edificios", [])),
    "piso": set(mapa.get("Pisos", [])),
    "recinto": set(mapa.get("Recintos", [])),
    "tipo": set(mapa.get("Tipos", [])),
}
listados = set(mapa.get("Dispositivos", []))
# El registro es la primera columna de la tabla de firmware/README.md.
numeros = re.findall(r"^\|\s*`(F\d{3})`", REGISTRO.read_text(encoding="utf-8"), re.M)

for nombre, cantidad in {n: numeros.count(n) for n in numeros}.items():
    if cantidad > 1:
        errores.append(f"firmware/README.md: {nombre} aparece {cantidad} veces")

carpetas_por_numero = {}
for carpeta in sorted(p for p in (RAIZ / "firmware").iterdir() if p.is_dir()):
    m = FIRMWARE.match(carpeta.name)
    if not m:
        errores.append(f"firmware/{carpeta.name}: el nombre tiene que ser Fnnn_descripcion")
        continue
    carpetas_por_numero.setdefault(f"F{m.group(1)}", []).append(carpeta.name)
    if not (carpeta / f"{carpeta.name}.ino").is_file():
        errores.append(f"firmware/{carpeta.name}: falta {carpeta.name}.ino")
    if f"F{m.group(1)}" not in numeros:
        errores.append(f"firmware/{carpeta.name}: F{m.group(1)} no está en el registro de firmware/README.md")
for numero, nombres in carpetas_por_numero.items():
    if len(nombres) > 1:
        errores.append(f"firmware/: {numero} lo usan {len(nombres)} carpetas ({', '.join(nombres)})")

for carpeta in sorted(p for p in (RAIZ / "dispositivos").iterdir() if p.is_dir()):
    if carpeta.name.startswith("_"):
        continue
    m = CODIGO.match(carpeta.name)
    if not m:
        errores.append(f"dispositivos/{carpeta.name}: el código tiene que ser EDIFICIO-PISO-RECINTO-TIPOnn")
        continue
    for clave, valor in zip(("edificio", "piso", "recinto", "tipo"), m.groups()[:4]):
        if valor not in partes[clave]:
            errores.append(f"dispositivos/{carpeta.name}: {clave} '{valor}' no está en el mapa de dispositivos/README.md")
    if m.group(5) == "00":
        errores.append(f"dispositivos/{carpeta.name}: la numeración empieza en 01")
    if carpeta.name not in listados:
        errores.append(f"dispositivos/{carpeta.name}: falta en la tabla 'Dispositivos' de dispositivos/README.md")
    ficha = carpeta / "README.md"
    if not ficha.is_file():
        errores.append(f"dispositivos/{carpeta.name}: falta la ficha README.md")
        continue
    # F001 o F001_descripcion: el guion bajo cuenta como letra para \b, así que no se usa.
    usados = set(re.findall(r"(?<![A-Za-z0-9])(F\d{3})(?!\d)", ficha.read_text(encoding="utf-8")))
    if not usados:
        errores.append(f"dispositivos/{carpeta.name}: la ficha no dice qué firmware (Fnnn) usa")
    for fw in sorted(usados - set(numeros)):
        errores.append(f"dispositivos/{carpeta.name}: {fw} no está en el registro de firmware/README.md")

for e in errores:
    print(f"::error::{e}")
print(f"{len(errores)} errores" if errores else "dispositivos/ y firmware/ en orden")
sys.exit(1 if errores else 0)
