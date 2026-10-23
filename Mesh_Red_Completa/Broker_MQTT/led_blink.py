"""
Prueba bidireccional AURA (contrato v4.0): prende y apaga el LED de una hoja cada 1 s.

  PC --MQTT--> gateway --malla--> (concentrador) --> hoja (LED GPIO8)
  PC <--MQTT-- gateway <--malla-- (concentrador) <-- hoja (resultado del comando)

Publica {"command":"led","params":{"encendido":...},"command_id":...} en
hw/<hw_id>/command y escucha hw/<hw_id>/response, midiendo el tiempo hasta "aplicado".

Uso:   python led_blink.py mac-907069add964                (broker en localhost)
       python led_blink.py mac-907069add964 192.168.1.5    (otro broker)
"""
import json
import socket
import sys
import threading
import time

if len(sys.argv) < 2:
    sys.exit("Uso: python led_blink.py <hw_id de la hoja, p. ej. mac-907069add964> [broker]")
HW_ID = sys.argv[1]
BROKER = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
TOPICO_CMD = f"hw/{HW_ID}/command"
TOPICO_RESP = f"hw/{HW_ID}/response"
PERIODO_S = 1.0

enviados = {}  # command_id -> hora de envío


def largo(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def cadena(s):
    b = s.encode()
    return len(b).to_bytes(2, "big") + b


def paquete(cab, cuerpo):
    return bytes([cab]) + largo(len(cuerpo)) + cuerpo


def leer_exacto(sock, n):
    buf = b""
    while len(buf) < n:
        parte = sock.recv(n - len(buf))
        if not parte:
            raise ConnectionError("broker cerró la conexión")
        buf += parte
    return buf


def leer_paquete(sock):
    cab = leer_exacto(sock, 1)[0]
    mult, n = 1, 0
    while True:
        b = leer_exacto(sock, 1)[0]
        n += (b & 0x7F) * mult
        if not b & 0x80:
            break
        mult *= 128
    return cab, leer_exacto(sock, n) if n else b""


def escuchar(sock):
    while True:
        cab, cuerpo = leer_paquete(sock)
        if cab >> 4 != 3:      # solo PUBLISH
            continue
        n = int.from_bytes(cuerpo[:2], "big")
        topico = cuerpo[2:2 + n].decode()
        payload = cuerpo[2 + n:].decode("utf-8", "replace")
        try:
            r = json.loads(payload)
            final = r.get("status") in ("aplicado", "rechazado")
            command_id = r.get("details", {}).get("command_id")
        except (ValueError, AttributeError):
            final, command_id = False, None
        t0 = enviados.pop(command_id, None) if final else None
        rtt = f"  ida y vuelta {1000 * (time.time() - t0):.0f} ms" if t0 else ""
        print(f"  <- {topico}  {payload}{rtt}", flush=True)


def main():
    sock = socket.create_connection((BROKER, 1883), timeout=10)
    sock.settimeout(None)
    # CONNECT (MQTT 3.1.1, sesión limpia, keepalive 60 s)
    sock.sendall(paquete(0x10, cadena("MQTT") + b"\x04\x02\x00\x3c" + cadena("aura-led-blink")))
    cab, cuerpo = leer_paquete(sock)
    if cab != 0x20 or cuerpo[1] != 0:
        sys.exit(f"El broker rechazó la conexión: {cuerpo!r}")
    # SUBSCRIBE a las respuestas de la hoja
    sock.sendall(paquete(0x82, b"\x00\x01" + cadena(TOPICO_RESP) + b"\x00"))
    threading.Thread(target=escuchar, args=(sock,), daemon=True).start()

    print(f"Broker {BROKER}:1883  ->  {TOPICO_CMD} cada {PERIODO_S} s  (Ctrl+C para salir)")
    estado, ultimo_ping, n = 1, time.time(), 0
    try:
        while True:
            n += 1
            command_id = f"blink-{n}"
            payload = json.dumps({"command": "led", "params": {"encendido": bool(estado)},
                                  "command_id": command_id}).encode()
            enviados[command_id] = time.time()
            sock.sendall(paquete(0x30, cadena(TOPICO_CMD) + payload))
            print(f"-> {TOPICO_CMD}  {payload.decode()}", flush=True)
            estado ^= 1
            if time.time() - ultimo_ping > 30:
                sock.sendall(b"\xc0\x00")  # PINGREQ
                ultimo_ping = time.time()
            time.sleep(PERIODO_S)
    except KeyboardInterrupt:
        sock.sendall(b"\xe0\x00")          # DISCONNECT
        sock.close()


if __name__ == "__main__":
    main()
