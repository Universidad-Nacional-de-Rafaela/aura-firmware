#!/usr/bin/env python3
"""Convierte un certificado PEM en el #define que va en credenciales.h del gateway.

Uso:  python pem_a_define.py ca.pem            (imprime #define OTA_CA_PEM ...)
      python pem_a_define.py ca.pem BROKER_CA_PEM
Copiar lo que imprime y pegarlo en main/credenciales.h, en lugar del OTA_CA_PEM comentado.
"""
import sys

if len(sys.argv) < 2:
    sys.exit(__doc__)
nombre = sys.argv[2] if len(sys.argv) > 2 else "OTA_CA_PEM"
lineas = [l.strip() for l in open(sys.argv[1], encoding="ascii").read().splitlines() if l.strip()]
if not lineas or not lineas[0].startswith("-----BEGIN CERTIFICATE-----"):
    sys.exit("No parece un certificado PEM: tiene que empezar con -----BEGIN CERTIFICATE-----")
if any(l.startswith("-----BEGIN") and "PRIVATE KEY" in l for l in lineas):
    sys.exit("Ese archivo es una clave privada: va el certificado de la CA (ca.pem), nunca ca.key")
print(f"#define {nombre} \\")
for i, l in enumerate(lineas):
    fin = "" if i == len(lineas) - 1 else " \\"
    print(f'    "{l}\\n"{fin}')
