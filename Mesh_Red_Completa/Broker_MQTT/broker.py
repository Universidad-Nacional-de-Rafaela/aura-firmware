"""
Broker MQTT mínimo para el banco de pruebas del Proyecto AURA (sin dependencias).

- MQTT 3.1.1: CONNECT, PUBLISH (QoS 0/1/2), SUBSCRIBE, UNSUBSCRIBE, PING, DISCONNECT.
- Mensajes retenidos (el status de cada placa es retenido, contrato §3.2) y Last Will
  (el gateway registra "offline" en su status). Controla el keepalive como un broker real: si un
  cliente pasa 1,5 x su keepalive sin mandar nada (se colgó o se apagó sin cerrar), se lo da
  por caído y se publica su Last Will.
- Imprime cada mensaje publicado (tópico + payload) y lo guarda en mensajes.log.
- Reenvía los mensajes a los clientes suscriptos (QoS 0).

- AURA falsa: como no hay backend en el banco, contesta cada hw/<hw_id>/data que trae
  ingest_id con el ack que publicaría AURA después de guardar (contrato §3.6):
  "persistido", o "duplicado" si ya vio ese ingest_id. Así el gateway confirma la muestra
  y la hoja la saca de su cola. El modo se cambia desde el tablero:
      ok          normal
      sin_ack     base caída: no contesta (la hoja guarda y reintenta 15 s ... 5 min)
      cuarentena  placa sin asignar: contesta cuarentena (la hoja libera la muestra)
      rechazado   contesta rechazado (la hoja libera la muestra)
  Con el backend real levantado, arrancar con --sin-aura: si no, cada muestra recibe dos ack.

- Tablero web en http://localhost:8080 (dashboard.html): las placas que publican en
  hw/<hw_id>/..., sus datos y su estado, y comandos (LED, aula, intervalo).

- Descubrimiento: contesta en UDP 18830 a "AURA_BROKER?" con "AURA_BROKER <puerto MQTT>".

Uso:   python broker.py               (MQTT en 0.0.0.0:1883, tablero en :8080)
       python broker.py 1884          (otro puerto MQTT)
       python broker.py --sin-aura    (sin la AURA falsa)

No tiene usuarios ni TLS: es solo para el laboratorio.
"""
import asyncio
import datetime
import itertools
import json
import os
import re
import sys
import time
from urllib.parse import parse_qs, urlparse

ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
PUERTO = int(ARGS[0]) if ARGS else 1883
PUERTO_WEB = 8080
PUERTO_DESCUBRIMIENTO = 18830        # UDP; tiene que coincidir con el gateway
CARPETA = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(CARPETA, "mensajes.log")
MODOS_AURA = ("ok", "sin_ack", "cuarentena", "rechazado", "apagada")
TOPICO_DATA = re.compile(r"^hw/((?:mac-[0-9a-f]{12})|(?:eui-[0-9a-f]{16})|(?:svc-[a-z0-9-]{1,32}))/data$")

suscripciones = {}   # writer -> set de filtros
oyentes_web = set()  # colas asyncio de los navegadores conectados (SSE)
historial = []       # últimos mensajes, para quien abre el tablero tarde
retenidos = {}       # tópico -> payload
modo_aura = "apagada" if "--sin-aura" in sys.argv else "ok"
vistos = set()       # ingest_id ya "guardados" por la AURA falsa
numero_comando = itertools.count(1)


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
    return buf[i + 2:i + 2 + n], i + 2 + n


def mostrar(cliente, topico, payload, retenido=False):
    texto = payload.decode("utf-8", "replace")
    try:
        texto_lindo = json.dumps(json.loads(texto), ensure_ascii=False)
    except ValueError:
        texto_lindo = texto
    linea = f"{ahora()}  [{cliente}]  {topico}  {texto_lindo}"
    print(linea, flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(datetime.date.today().isoformat() + " " + linea + "\n")
    evento = json.dumps({"t": time.time(), "cliente": cliente, "topico": topico, "payload": texto,
                         "retenido": retenido})
    historial.append(evento)
    del historial[:-300]
    for cola in list(oyentes_web):
        cola.put_nowait(evento)


def paquete_publish(topico, payload, retener=False):
    t = topico.encode()
    cuerpo = len(t).to_bytes(2, "big") + t + payload
    return bytes([0x31 if retener else 0x30]) + codificar_largo(len(cuerpo)) + cuerpo


async def reenviar(topico, payload):
    paquete = paquete_publish(topico, payload)
    for w, filtros in list(suscripciones.items()):
        if any(coincide(f, topico) for f in filtros):
            try:
                w.write(paquete)
                await w.drain()
            except (ConnectionError, OSError):
                suscripciones.pop(w, None)


async def publicar(cliente, topico, payload, retener=False):
    """Lo que publica un cliente, el tablero, el Last Will o la AURA falsa."""
    if retener:
        if payload:
            retenidos[topico] = payload
        else:
            retenidos.pop(topico, None)
    mostrar(cliente, topico, payload)
    await reenviar(topico, payload)
    await aura_falsa(topico, payload)


async def aura_falsa(topico, payload):
    """El ack que publicaría AURA después de guardar un data (contrato §3.6)."""
    m = TOPICO_DATA.match(topico)
    if not m or modo_aura in ("sin_ack", "apagada"):
        return
    try:
        datos = json.loads(payload)
    except ValueError:
        return
    ingest_id = datos.get("ingest_id")
    if not isinstance(ingest_id, str) or not ingest_id:
        return
    if not isinstance(datos.get("values"), dict) or modo_aura == "rechazado":
        ack = {"ingest_id": ingest_id, "resultado": "rechazado",
               "motivo": "modo_rechazado" if modo_aura == "rechazado" else "values_invalido"}
    elif ingest_id in vistos:
        ack = {"ingest_id": ingest_id, "resultado": "duplicado"}
    else:
        vistos.add(ingest_id)
        ack = {"ingest_id": ingest_id, "resultado": "cuarentena" if modo_aura == "cuarentena" else "persistido"}
    await publicar("aura-falsa", f"hw/{m.group(1)}/ack", json.dumps(ack).encode())


async def atender(reader, writer):
    ip = writer.get_extra_info("peername")[0]
    cliente = ip
    voluntad = None          # (tópico, payload, retener) del Last Will
    espera = None            # 1,5 x keepalive; None = sin límite
    try:
        while True:
            try:
                cab, cuerpo = await asyncio.wait_for(leer_paquete(reader), espera)
            except asyncio.TimeoutError:
                print(f"{ahora()}  ! {cliente}: venció el keepalive", flush=True)
                break
            tipo = cab >> 4

            if tipo == 1:  # CONNECT
                _, i = leer_str(cuerpo, 0)          # "MQTT"
                flags = cuerpo[i + 1]
                keepalive = int.from_bytes(cuerpo[i + 2:i + 4], "big")
                espera = keepalive * 1.5 if keepalive else None
                i += 1 + 1 + 2                      # nivel, flags, keepalive
                client_id, i = leer_str(cuerpo, i)
                cliente = f"{client_id.decode('utf-8', 'replace') or '?'}@{ip}"
                if flags & 0x04:                    # Last Will
                    w_topico, i = leer_str(cuerpo, i)
                    w_payload, i = leer_str(cuerpo, i)
                    voluntad = (w_topico.decode(), w_payload, bool(flags & 0x20))
                print(f"{ahora()}  + conectado {cliente}", flush=True)
                writer.write(b"\x20\x02\x00\x00")   # CONNACK aceptado

            elif tipo == 3:  # PUBLISH
                qos = (cab >> 1) & 0x03
                topico, i = leer_str(cuerpo, 0)
                pid = b""
                if qos:
                    pid = cuerpo[i:i + 2]
                    i += 2
                if qos == 1:
                    writer.write(b"\x40\x02" + pid)  # PUBACK
                elif qos == 2:
                    writer.write(b"\x50\x02" + pid)  # PUBREC
                await writer.drain()
                await publicar(cliente, topico.decode("utf-8", "replace"), cuerpo[i:], bool(cab & 0x01))

            elif tipo == 6:  # PUBREL
                writer.write(b"\x70\x02" + cuerpo[:2])  # PUBCOMP

            elif tipo == 8:  # SUBSCRIBE
                pid, i, otorgados, nuevos = cuerpo[:2], 2, bytearray(), []
                filtros = suscripciones.setdefault(writer, set())
                while i < len(cuerpo):
                    f, i = leer_str(cuerpo, i)
                    f = f.decode()
                    i += 1                          # QoS pedida
                    filtros.add(f)
                    nuevos.append(f)
                    otorgados.append(0)
                    print(f"{ahora()}  {cliente} suscripto a {f}", flush=True)
                resto = pid + bytes(otorgados)
                writer.write(bytes([0x90]) + codificar_largo(len(resto)) + resto)
                for t, p in list(retenidos.items()):          # lo retenido, al suscribirse
                    if any(coincide(f, t) for f in nuevos):
                        writer.write(paquete_publish(t, p, retener=True))

            elif tipo == 10:  # UNSUBSCRIBE
                pid, i = cuerpo[:2], 2
                while i < len(cuerpo):
                    f, i = leer_str(cuerpo, i)
                    suscripciones.get(writer, set()).discard(f.decode())
                writer.write(b"\xb0\x02" + pid)

            elif tipo == 12:  # PINGREQ
                writer.write(b"\xd0\x00")

            elif tipo == 14:  # DISCONNECT: desconexión prolija, sin Last Will
                voluntad = None
                break

            await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError, OSError):
        pass
    finally:
        suscripciones.pop(writer, None)
        print(f"{ahora()}  - desconectado {cliente}", flush=True)
        writer.close()
        if voluntad:
            await publicar(f"last-will {cliente}", voluntad[0], voluntad[1], voluntad[2])


# ---------------------------------------------------------------------------
#  Tablero web (HTTP + Server-Sent Events)
# ---------------------------------------------------------------------------
async def atender_web(reader, writer):
    global modo_aura
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
            for t, p in retenidos.items():          # primero el estado retenido de cada placa
                cola.put_nowait(json.dumps({"t": time.time(), "cliente": "retenido", "topico": t,
                                            "payload": p.decode("utf-8", "replace"), "retenido": True}))
            for ev in historial:
                cola.put_nowait(ev)
            cola.put_nowait(json.dumps({"aura": modo_aura}))
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
            # /cmd?hw=mac-...&command=led&params={"encendido":true}
            hw = q.get("hw", "")
            params = json.loads(q.get("params", "{}"))
            orden = {"command": q.get("command", ""), "params": params,
                     "command_id": f"tablero-{next(numero_comando)}"}
            await publicar("tablero", f"hw/{hw}/command", json.dumps(orden).encode())
            writer.write(b"HTTP/1.1 204 No Content\r\n\r\n")

        elif url.path == "/aura" and metodo == "POST":
            if q.get("modo") in MODOS_AURA:
                modo_aura = q["modo"]
                print(f"{ahora()}  AURA falsa: modo {modo_aura}", flush=True)
                for cola in list(oyentes_web):
                    cola.put_nowait(json.dumps({"aura": modo_aura}))
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
    print(f"AURA falsa (ack): {modo_aura}")
    print(f"Tablero: http://localhost:{PUERTO_WEB}")
    print(f"Log: {LOG}", flush=True)
    async with servidor, web:
        await asyncio.gather(servidor.serve_forever(), web.serve_forever())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
