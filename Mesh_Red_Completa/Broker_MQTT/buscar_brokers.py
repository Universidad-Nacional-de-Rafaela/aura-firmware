#!/usr/bin/env python3
"""Pregunta en la red WiFi, como el gateway, quién contesta como broker de AURA.

Hace lo mismo que el gateway con DESCUBRIR_BROKER 1: manda "AURA_BROKER?" por broadcast al
puerto UDP 18830 y junta las respuestas ("AURA_BROKER <puerto>"). El gateway se queda con la
PRIMERA que llega, así que si contesta alguien que no es tu PC, se podría conectar ahí.

Uso, con broker.py APAGADO en esta PC:
    python buscar_brokers.py

Pregunta a 255.255.255.255 y a la dirección de broadcast de la red de la PC. Esa dirección se
arma suponiendo una red /24 (x.y.z.255), como la del banco; en una red más grande, la pregunta
a 255.255.255.255 igual llega a toda la red local.
"""
import socket
import time

PUERTO = 18830
PREGUNTA = b"AURA_BROKER?"


def mis_ips():
    """Las IP de esta PC. Un socket UDP "conectado" a una dirección pública no manda nada, pero
    dice por qué IP saldría: anda en Windows y en Linux, donde el nombre de la PC suele resolver a
    127.0.1.1. Sin red hacia afuera, queda lo que resuelve el nombre de la PC."""
    ips = set()
    try:
        t = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        t.connect(("8.8.8.8", 80))
        ips.add(t.getsockname()[0])
        t.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    return {ip for ip in ips if not ip.startswith("127.")}


def main():
    propias = mis_ips()
    # x.y.z.255 supone una red /24 (ver arriba)
    destinos = {"255.255.255.255"} | {ip.rsplit(".", 1)[0] + ".255" for ip in propias}
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("", 0))
    s.settimeout(0.5)
    vistos = {}
    for ronda in range(3):                         # 3 preguntas, por si se pierde alguna
        for d in destinos:
            try:
                s.sendto(PREGUNTA, (d, PUERTO))
            except OSError:
                pass
        fin = time.time() + 2
        while time.time() < fin:
            try:
                datos, (ip, _) = s.recvfrom(64)
            except socket.timeout:
                continue
            texto = datos.decode(errors="replace").strip()
            if texto.startswith("AURA_BROKER") and texto != "AURA_BROKER?":
                vistos[ip] = texto
    s.close()
    print(f"IPs de esta PC: {', '.join(sorted(propias)) or '?'}")
    if not vistos:
        print("Nadie contestó: no hay otro broker de AURA buscable en esta red.")
        return
    for ip, texto in sorted(vistos.items()):
        quien = "ESTA PC (¿broker.py está corriendo?)" if ip in propias else "OTRA COMPUTADORA"
        print(f"Contestó {ip}: '{texto}'  ->  {quien}")
    if any(ip not in propias for ip in vistos):
        print("\nHay otro broker en la red: el gateway se podría conectar a ese. Conviene fijarle la dirección.")


if __name__ == "__main__":
    main()
