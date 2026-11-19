#!/usr/bin/env python3
"""Firma una imagen de ESP-IDF con la clave de prueba y verifica la firma.

Uso (en la terminal de ESP-IDF, desde la carpeta del proyecto):
  python firmar.py --clave C:\\aura-claves\\firma_prueba.pem build\\aura-energia-pm2130.bin
      firma esa imagen EN EL LUGAR (la que despues graba "idf.py flash")
  python firmar.py --clave C:\\aura-claves\\firma_prueba.pem build\\aura-energia-pm2130.bin C:\\aura-imagenes\\tab01-1.0.1.bin
      firma una COPIA y deja el original sin firmar

--clave es obligatorio: la clave privada vive FUERA del repo (por ejemplo en C:\\aura-claves) y el
script nunca la busca por su cuenta, para que no termine al lado del codigo ni en un commit.

Hace lo de GUIA_NODOS_HOJA.md 2.9 (espsecure sign-data + verify-signature) sin mover archivos a mano.
"""
import os
import subprocess
import sys

args = sys.argv[1:]
clave = None
if "--clave" in args:
    i = args.index("--clave")
    if i + 1 >= len(args):
        sys.exit("Falta la ruta despues de --clave.\n\n" + __doc__)
    clave = args[i + 1]
    del args[i:i + 2]
if clave is None or not args or len(args) > 2:
    sys.exit(__doc__)
origen = args[0]
destino = args[1] if len(args) == 2 else origen
if not os.path.isfile(origen):
    sys.exit(f"No existe {origen}: ¿compilaste con idf.py build? ¿estás en la carpeta del proyecto?")
if not os.path.isfile(clave):
    sys.exit(f"No existe la clave {clave}: generala con el paso de las claves")

# Firmar dos veces la misma imagen la deforma (espsecure no avisa): si ya tiene firma, no se toca.
info = subprocess.run([sys.executable, "-m", "espsecure", "signature-info-v2", origen],
                      capture_output=True, text=True)
if "Signature block 0 is valid" in info.stdout + info.stderr:
    misma = subprocess.run([sys.executable, "-m", "espsecure", "verify-signature", "--version", "2",
                            "--keyfile", clave, origen], capture_output=True, text=True).returncode == 0
    if misma and destino == origen:
        print(f"{origen} ya estaba firmada con {os.path.basename(clave)}: está lista, no se vuelve a firmar.")
        sys.exit(0)
    sys.exit(f"{origen} YA TIENE UNA FIRMA: no se vuelve a firmar. Borrá ese .bin, compilá de nuevo con "
             "idf.py build y firmá la imagen recién compilada.")

temporal = destino + ".firmando"
r = subprocess.run([sys.executable, "-m", "espsecure", "sign-data", "--version", "2",
                    "--keyfile", clave, "--output", temporal, origen])
if r.returncode != 0:
    sys.exit("No se pudo firmar (mirá el mensaje de arriba). Si dice que ya está firmada, compilá de nuevo.")
os.replace(temporal, destino)
r = subprocess.run([sys.executable, "-m", "espsecure", "verify-signature", "--version", "2",
                    "--keyfile", clave, destino], capture_output=True, text=True)
if r.returncode != 0:
    sys.exit("La firma NO verifica:\n" + r.stdout + r.stderr)
print(f"Firmada y verificada: {destino}  (clave {os.path.basename(clave)})")
