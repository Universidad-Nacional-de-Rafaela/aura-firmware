#!/usr/bin/env python3
"""Pregunta en la red WiFi, como el gateway, quién contesta como broker de AURA.

Hace lo mismo que el gateway con DESCUBRIR_BROKER 1: manda "AURA_BROKER?" por broadcast al
puerto UDP 18830 y junta las respuestas ("AURA_BROKER <puerto>"). El gateway se queda con la
PRIMERA que llega, así que si contesta alguien que no es tu PC, se podría conectar ahí.

Uso, con broker.py APAGADO en esta PC:
    python buscar_brokers.py
"""
import socket
import time

PUERTO = 18830
PREGUNTA = b"AURA_BROKER?"


def mis_ips():
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    return {ip for ip in ips if not ip.startswith("127.")}


def main():
    propias = mis_ips()
    destinos = {"255.255.255.255"} | {ip.rsplit(".", 1)[0] + ".255" for ip in propias if not ip.startswith("127.")}
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
