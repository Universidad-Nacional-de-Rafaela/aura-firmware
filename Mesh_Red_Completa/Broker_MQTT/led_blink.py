"""
Prueba bidireccional AURA: prende y apaga el LED de la hoja cada 1 s por MQTT.

  PC --MQTT--> gateway --malla--> concentrador --> hoja (LED GPIO8)
  PC <--MQTT-- gateway <--malla-- concentrador <-- hoja (responde el estado)

Publica {"led":1} / {"led":0} en aura/cmd/<id de la hoja> y escucha la
respuesta en aura/<aula>/led, midiendo el tiempo de ida y vuelta.

Uso:   python led_blink.py                         (broker en localhost)
       python led_blink.py 192.168.1.5             (otro broker)
       python led_blink.py 127.0.0.1 aula2-hoja    (otra hoja)
"""
import json
import socket
import sys
import threading
import time

BROKER = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
ID_HOJA = sys.argv[2] if len(sys.argv) > 2 else "aula1-hoja"
TOPICO_CMD = f"aura/cmd/{ID_HOJA}"
TOPICO_RESP = f"aura/{ID_HOJA.split('-')[0]}/led"   # aula1-hoja -> aura/aula1/led
PERIODO_S = 1.0

enviados = {}  # estado del LED -> hora de envío del último comando


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
            estado = json.loads(payload).get("led")
        except ValueError:
            estado = None
        t0 = enviados.pop(estado, None)
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
    estado, ultimo_ping = 1, time.time()
    try:
        while True:
            payload = json.dumps({"led": estado}).encode()
            enviados[estado] = time.time()
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
