"""
Broker MQTT mínimo para pruebas del Proyecto AURA (sin dependencias).

- MQTT 3.1.1: CONNECT, PUBLISH (QoS 0/1/2), SUBSCRIBE, UNSUBSCRIBE, PING, DISCONNECT.
- Imprime cada mensaje publicado (tópico + payload) y lo guarda en mensajes.log.
- Reenvía los mensajes a los clientes suscriptos (QoS 0), así se puede
  mirar con cualquier cliente (MQTT Explorer, mosquitto_sub, etc.).

- Tablero web en http://localhost:8080 (dashboard.html): muestra en vivo lo
  que llega por MQTT y permite mandar comandos al LED de la hoja.

- Descubrimiento: contesta en UDP 18830 a la pregunta "AURA_BROKER?" con
  "AURA_BROKER <puerto MQTT>". El gateway la manda por broadcast al conectarse,
  así encuentra la IP de la PC solo, sin configurarla en el firmware.

Uso:   python broker.py            (MQTT en 0.0.0.0:1883, tablero en :8080)
       python broker.py 1884       (otro puerto MQTT)

No tiene usuarios, TLS ni mensajes retenidos: es solo para el laboratorio.
"""
import asyncio
import datetime
import json
import os
import sys
import time
from urllib.parse import parse_qs, urlparse

PUERTO = int(sys.argv[1]) if len(sys.argv) > 1 else 1883
PUERTO_WEB = 8080
PUERTO_DESCUBRIMIENTO = 18830        # UDP; tiene que coincidir con el gateway
CARPETA = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(CARPETA, "mensajes.log")
DESTINO = "aula1-hoja"               # id lógico del nodo que comanda el tablero

suscripciones = {}  # writer -> set de filtros
oyentes_web = set()  # colas asyncio de los navegadores conectados (SSE)
historial = []       # últimos mensajes, para quien abre el tablero tarde
parpadeo = None      # tarea del modo "parpadear"


def ahora():
    return datetime.datetime.now().strftime("%H:%M:%S")


def codificar_largo(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def coincide(filtro, topico):
    f = filtro.split("/")
    t = topico.split("/")
    for i, parte in enumerate(f):
        if parte == "#":
            return True
        if i >= len(t):
            return False
        if parte != "+" and parte != t[i]:
            return False
    return len(f) == len(t)


async def leer_paquete(reader):
    cab = (await reader.readexactly(1))[0]
    mult, largo = 1, 0
    while True:
        b = (await reader.readexactly(1))[0]
        largo += (b & 0x7F) * mult
        if not b & 0x80:
            break
        mult *= 128
    cuerpo = await reader.readexactly(largo) if largo else b""
    return cab, cuerpo


def leer_str(buf, i):
    n = int.from_bytes(buf[i:i + 2], "big")
    return buf[i + 2:i + 2 + n].decode("utf-8", "replace"), i + 2 + n


def mostrar(cliente, topico, payload):
    texto = payload.decode("utf-8", "replace")
    try:
        texto_lindo = json.dumps(json.loads(texto), ensure_ascii=False)
    except ValueError:
        texto_lindo = texto
    linea = f"{ahora()}  [{cliente}]  {topico}  {texto_lindo}"
    print(linea, flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(datetime.date.today().isoformat() + " " + linea + "\n")
    evento = json.dumps({"t": time.time(), "cliente": cliente, "topico": topico, "payload": texto})
    historial.append(evento)
    del historial[:-200]
    for cola in list(oyentes_web):
        cola.put_nowait(evento)


async def reenviar(topico, payload):
    t = topico.encode()
    cuerpo = len(t).to_bytes(2, "big") + t + payload
    paquete = bytes([0x30]) + codificar_largo(len(cuerpo)) + cuerpo
    for w, filtros in list(suscripciones.items()):
        if any(coincide(f, topico) for f in filtros):
            try:
                w.write(paquete)
                await w.drain()
            except (ConnectionError, OSError):
                suscripciones.pop(w, None)


async def atender(reader, writer):
    ip = writer.get_extra_info("peername")[0]
    cliente = ip
    try:
        while True:
            cab, cuerpo = await leer_paquete(reader)
            tipo = cab >> 4

            if tipo == 1:  # CONNECT
                _, i = leer_str(cuerpo, 0)          # "MQTT"
                i += 1 + 1 + 2                      # nivel, flags, keepalive
                client_id, _ = leer_str(cuerpo, i)
                cliente = f"{client_id or '?'}@{ip}"
                print(f"{ahora()}  + conectado {cliente}", flush=True)
                writer.write(b"\x20\x02\x00\x00")   # CONNACK aceptado

            elif tipo == 3:  # PUBLISH
                qos = (cab >> 1) & 0x03
                topico, i = leer_str(cuerpo, 0)
                pid = b""
                if qos:
                    pid = cuerpo[i:i + 2]
                    i += 2
                payload = cuerpo[i:]
                mostrar(cliente, topico, payload)
                if qos == 1:
                    writer.write(b"\x40\x02" + pid)  # PUBACK
                elif qos == 2:
                    writer.write(b"\x50\x02" + pid)  # PUBREC
                await reenviar(topico, payload)

            elif tipo == 6:  # PUBREL
                writer.write(b"\x70\x02" + cuerpo[:2])  # PUBCOMP

            elif tipo == 8:  # SUBSCRIBE
                pid, i, otorgados = cuerpo[:2], 2, bytearray()
                filtros = suscripciones.setdefault(writer, set())
                while i < len(cuerpo):
                    f, i = leer_str(cuerpo, i)
                    i += 1                          # QoS pedida
                    filtros.add(f)
                    otorgados.append(0)
                    print(f"{ahora()}  {cliente} suscripto a {f}", flush=True)
                resto = pid + bytes(otorgados)
                writer.write(bytes([0x90]) + codificar_largo(len(resto)) + resto)

            elif tipo == 10:  # UNSUBSCRIBE
                pid, i = cuerpo[:2], 2
                while i < len(cuerpo):
                    f, i = leer_str(cuerpo, i)
                    suscripciones.get(writer, set()).discard(f)
                writer.write(b"\xb0\x02" + pid)

            elif tipo == 12:  # PINGREQ
                writer.write(b"\xd0\x00")

            elif tipo == 14:  # DISCONNECT
                break

            await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError, OSError):
        pass
    finally:
        suscripciones.pop(writer, None)
        print(f"{ahora()}  - desconectado {cliente}", flush=True)
        writer.close()


# ---------------------------------------------------------------------------
#  Tablero web (HTTP + Server-Sent Events)
# ---------------------------------------------------------------------------
async def publicar_desde_web(topico, payload):
    mostrar("tablero", topico, payload)
    await reenviar(topico, payload)


async def bucle_parpadeo(destino, periodo):
    estado = 1
    while True:
        await publicar_desde_web(f"aura/cmd/{destino}", json.dumps({"led": estado}).encode())
        estado ^= 1
        await asyncio.sleep(periodo)


async def atender_web(reader, writer):
    global parpadeo
    try:
        linea = (await reader.readline()).decode("latin-1")
        while (await reader.readline()) not in (b"\r\n", b"\n", b""):
            pass                                    # se ignoran los encabezados
        metodo, ruta, _ = linea.split(" ", 2)
        url = urlparse(ruta)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}

        if url.path == "/":
            with open(os.path.join(CARPETA, "dashboard.html"), "rb") as f:
                cuerpo = f.read()
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                         b"Content-Length: " + str(len(cuerpo)).encode() + b"\r\n\r\n" + cuerpo)

        elif url.path == "/eventos":
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                         b"Cache-Control: no-cache\r\n\r\n")
            cola = asyncio.Queue()
            for ev in historial:
                cola.put_nowait(ev)
            oyentes_web.add(cola)
            try:
                while True:
                    try:
                        ev = await asyncio.wait_for(cola.get(), 15)
                        writer.write(f"data: {ev}\n\n".encode())
                    except asyncio.TimeoutError:
                        writer.write(b": ping\n\n")
                    await writer.drain()
            finally:
                oyentes_web.discard(cola)

        elif url.path == "/cmd" and metodo == "POST":
            destino = q.get("id", DESTINO)
            accion = q.get("led", "toggle")
            if parpadeo:
                parpadeo.cancel()
                parpadeo = None
            if accion == "blink":
                parpadeo = asyncio.create_task(bucle_parpadeo(destino, float(q.get("periodo", "1"))))
            elif accion != "stop":
                valor = {"1": 1, "0": 0}.get(accion, "toggle")
                await publicar_desde_web(f"aura/cmd/{destino}", json.dumps({"led": valor}).encode())
            writer.write(b"HTTP/1.1 204 No Content\r\n\r\n")

        else:
            writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        await writer.drain()
    except (ConnectionError, OSError, ValueError):
        pass
    finally:
        writer.close()


# ---------------------------------------------------------------------------
#  Descubrimiento del broker (UDP)
# ---------------------------------------------------------------------------
class Descubrimiento(asyncio.DatagramProtocol):
    def connection_made(self, transporte):
        self.transporte = transporte

    def datagram_received(self, datos, origen):
        if datos.strip() == b"AURA_BROKER?":
            self.transporte.sendto(f"AURA_BROKER {PUERTO}".encode(), origen)
            print(f"{ahora()}  descubrimiento: {origen[0]} preguntó por el broker, se le contestó", flush=True)


async def main():
    servidor = await asyncio.start_server(atender, "0.0.0.0", PUERTO)
    web = await asyncio.start_server(atender_web, "0.0.0.0", PUERTO_WEB)
    await asyncio.get_running_loop().create_datagram_endpoint(
        Descubrimiento, local_addr=("0.0.0.0", PUERTO_DESCUBRIMIENTO))
    print(f"Broker MQTT de prueba escuchando en 0.0.0.0:{PUERTO}  (Ctrl+C para salir)")
    print(f"Descubrimiento: UDP {PUERTO_DESCUBRIMIENTO}")
    print(f"Tablero: http://localhost:{PUERTO_WEB}")
    print(f"Log: {LOG}", flush=True)
    async with servidor, web:
        await asyncio.gather(servidor.serve_forever(), web.serve_forever())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
