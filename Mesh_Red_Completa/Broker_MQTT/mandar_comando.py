#!/usr/bin/env python3
"""Manda un comando a una placa a través de broker.py y muestra la respuesta, como el tablero.

Uso (en la PC donde corre broker.py, con broker.py corriendo):
    python mandar_comando.py <hw_id> <comando> [clave=valor ...] [clave:=numero ...]

    Solo para placas de la mesh (hw_id mac-…): las de LoRaWAN (eui-…) no pasan por broker.py.

    clave=valor    va como texto        (codigo=12AB34CD ->  "codigo": "12AB34CD")
    clave:=valor   va como número/JSON  (intervalo_s:=30 ->  "intervalo_s": 30)

Ejemplo:
    python mandar_comando.py mac-0123456789ab mi_comando codigo=12AB34CD tiempo_ms:=5000

Opciones: --broker <ip>   (por defecto 127.0.0.1, el broker.py de esta PC; si en la PC hay un
                          Mosquitto instalado como servicio, usar la IP de la PC en la red)
          --esperar <s>   (por defecto 40: cuánto espera la respuesta de la placa)
Los parámetros se escriben sin comillas ni llaves, así no hay problemas de comillas en PowerShell.
"""
import json
import socket
import sys
import time


def largo(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | 0x80 if n else b)
        if not n:
            return bytes(out)


def cadena(s):
    b = s.encode()
    return len(b).to_bytes(2, "big") + b


def paquete(cab, cuerpo):
    return bytes([cab]) + largo(len(cuerpo)) + cuerpo


def leer_exacto(sock, n):
    datos = b""
    while len(datos) < n:
        parte = sock.recv(n - len(datos))
        if not parte:
            raise ConnectionError("el broker cerró la conexión")
        datos += parte
    return datos


def leer_paquete(sock):
    cab = leer_exacto(sock, 1)[0]
    mult, n = 1, 0
    while True:
        b = leer_exacto(sock, 1)[0]
        n += (b & 0x7F) * mult
        mult *= 128
        if not b & 0x80:
            break
    return cab, leer_exacto(sock, n) if n else b""


def main():
    args = sys.argv[1:]
    broker, esperar = "127.0.0.1", 40.0
    for opcion in ("--broker", "--esperar"):
        if opcion in args:
            i = args.index(opcion)
            if i + 1 >= len(args):
                sys.exit(f"Falta el valor de {opcion}.\n\n{__doc__}")
            valor = args[i + 1]
            del args[i:i + 2]
            if opcion == "--broker":
                broker = valor
            else:
                esperar = float(valor)
    if len(args) >= 1 and args[0].startswith("eui-"):
        sys.exit(f"{args[0]} es una placa LoRaWAN: no pasa por broker.py. Este script es para las "
                 "placas de la mesh (mac-…).")
    if len(args) < 2 or not args[0].startswith("mac-"):
        sys.exit(__doc__)
    hw, comando, params = args[0], args[1], {}
    for p in args[2:]:
        if ":=" in p:
            k, v = p.split(":=", 1)
            try:
                params[k] = json.loads(v)
            except json.JSONDecodeError:
                sys.exit(f"'{v}' no es un número ni JSON válido (en {p}). Para texto, usá {k}={v}")
        elif "=" in p:
            k, v = p.split("=", 1)
            params[k] = v
        else:
            sys.exit(f"No entiendo '{p}': tiene que ser clave=valor o clave:=numero")
    command_id = f"manual-{int(time.time())}"
    mensaje = json.dumps({"command": comando, "params": params, "command_id": command_id},
                         ensure_ascii=False, separators=(",", ":"))

    sock = socket.create_connection((broker, 1883), timeout=5)
    sock.sendall(paquete(0x10, cadena("MQTT") + b"\x04\x02\x00\x3c" + cadena(f"comando-{int(time.time())}")))
    cab, cuerpo = leer_paquete(sock)
    if cab != 0x20 or cuerpo[1:2] != b"\x00":
        sys.exit("El broker rechazó la conexión (¿broker.py con usuario y clave?)")
    respuesta = f"hw/{hw}/response"
    sock.sendall(paquete(0x82, b"\x00\x01" + cadena(respuesta) + b"\x00"))
    leer_paquete(sock)                                        # SUBACK
    sock.sendall(paquete(0x30, cadena(f"hw/{hw}/command") + mensaje.encode()))
    print(f"Mandado a hw/{hw}/command:\n  {mensaje}\nEspero la respuesta de la placa (hasta {esperar:.0f} s)...")

    fin = time.time() + esperar
    final = False
    while time.time() < fin and not final:
        sock.settimeout(max(0.1, min(5.0, fin - time.time())))
        try:
            cab, cuerpo = leer_paquete(sock)
        except socket.timeout:
            sock.sendall(b"\xc0\x00")                         # PINGREQ, para mantener la conexión
            continue
        if cab >> 4 != 3:
            continue
        n = int.from_bytes(cuerpo[:2], "big")
        topico, carga = cuerpo[2:2 + n].decode(), cuerpo[2 + n:]
        if cab & 0x06:                                        # QoS > 0: salteo el id de paquete
            carga = carga[2:]
        try:
            r = json.loads(carga)
        except json.JSONDecodeError:
            continue
        if (r.get("details") or {}).get("command_id") != command_id:
            continue
        estado = r.get("status")
        print(f"{time.strftime('%H:%M:%S')}  {estado}: {json.dumps(r.get('details'), ensure_ascii=False)}")
        final = estado in ("aplicado", "rechazado")
    if not final:
        print("No llegó un aplicado ni un rechazado. ¿La placa está en línea? ¿Conoce ese comando?")
    sock.sendall(b"\xe0\x00")                                 # DISCONNECT
    sock.close()


if __name__ == "__main__":
    main()
