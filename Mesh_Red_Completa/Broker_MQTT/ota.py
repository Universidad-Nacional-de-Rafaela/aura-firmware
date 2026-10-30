"""
Actualiza placas de la mesh de AURA por la malla (OTA). En producción lo hará AURA con los
mismos mensajes; acá lo hace este script, en la PC del banco.

  PC: sirve el .bin por HTTP --> gateway: lo descarga una vez y lo verifica (SHA-256)
                                 gateway --malla--> una placa por vez --> verifica, se reinicia
  PC <--MQTT-- hw/<hw_id>/response: transmitido, recibido, aplicado (volvió con la versión nueva)
                                    o rechazado (details.motivo)

Elige las placas de dos formas:
  python ota.py build/aura-hoja-ejemplo.bin --hw mac-907069add964          una (o varias, --hw otra vez)
  python ota.py build/aura-hoja-ejemplo.bin --todas                         todas las de ese tipo
  python ota.py build/aura-hoja-ejemplo.bin --todas --aula aula11           todas las de ese tipo y esa aula

"Ese tipo" sale de la imagen (el project() con que se compiló) y de lo que cada placa informa en su
status (details.firmware.tipo). Las que ya tienen esa versión contestan aplicado con sin_cambios.
El gateway se actualiza a sí mismo igual (--hw con su hw_id y una imagen aura-gateway), y si en la
cola hay otras placas, lo deja para el final.

Opciones:
  --broker 127.0.0.1       dónde está el broker MQTT (puerto 1883)
  --ip 192.168.1.5         la IP de esta PC que ve el gateway (si no, se deduce)
  --puerto-http 8070       puerto del servidor de la imagen
  --forzar-tipo            instalarla aunque la placa sea de otro tipo (cambio de uso, a propósito)
  --esperar 1800           segundos máximos de espera

Si el gateway se reinicia o se corta el broker en el medio, lo que no terminó se vuelve a pedir (es
idempotente: una placa que ya tiene la versión contesta aplicado con sin_cambios).
Sin dependencias: solo la biblioteca estándar de Python.
"""
import argparse
import hashlib
import http.server
import json
import os
import re
import socket
import struct
import sys
import threading
import time

ESTADOS_FINALES = ("aplicado", "rechazado")
REGLA_TIPO = re.compile(r"^aura-[a-z0-9-]*[a-z0-9]$")
REGLA_VERSION = re.compile(r"^\d+\.\d+\.\d+$")


def ahora():
    return time.strftime("%H:%M:%S")


def log(txt):
    print(f"{ahora()}  {txt}", flush=True)


# ---------------------------------------------------------------------------
#  La imagen: tipo, versión y chip, de su encabezado (esp_image_header_t + esp_app_desc_t)
# ---------------------------------------------------------------------------
CHIPS = {0: "esp32", 2: "esp32s2", 5: "esp32c3", 9: "esp32s3", 12: "esp32c2", 13: "esp32c6", 16: "esp32h2"}


def leer_imagen(ruta):
    datos = open(ruta, "rb").read()
    if len(datos) < 32 + 256 or datos[0] != 0xE9:
        sys.exit(f"{ruta} no es una imagen de ESP-IDF")
    chip = struct.unpack_from("<H", datos, 12)[0]
    desc = 32                                  # cabecera (24) + primer segmento (8)
    if struct.unpack_from("<I", datos, desc)[0] != 0xABCD5432:
        sys.exit(f"{ruta} no tiene la descripción de la aplicación (esp_app_desc_t)")
    version = datos[desc + 16:desc + 48].split(b"\0")[0].decode()
    tipo = datos[desc + 48:desc + 80].split(b"\0")[0].decode()
    if not REGLA_TIPO.match(tipo) or len(tipo) > 31:
        sys.exit(f"El tipo de la imagen es '{tipo}': tiene que ser project(aura-<uso>) (GUIA_NODOS_HOJA.md)")
    if not REGLA_VERSION.match(version):
        sys.exit(f"La versión de la imagen es '{version}': tiene que ser set(PROJECT_VER \"mayor.menor.parche\")")
    return datos, tipo, version, CHIPS.get(chip, f"chip {chip}")


# ---------------------------------------------------------------------------
#  Servidor HTTP: solo esa imagen
# ---------------------------------------------------------------------------
def servir(datos, nombre, puerto):
    class Manejador(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != "/" + nombre:
                self.send_error(404)
                return
            log(f"HTTP: {self.client_address[0]} descarga la imagen ({len(datos)} bytes)")
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(datos)))
            self.end_headers()
            try:
                self.wfile.write(datos)
            except (ConnectionError, OSError) as e:
                log(f"HTTP: la descarga se cortó ({e})")

        def log_message(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", puerto), Manejador)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def mi_ip(hacia):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((hacia if hacia not in ("127.0.0.1", "localhost") else "8.8.8.8", 9))
        return s.getsockname()[0]
    finally:
        s.close()


# ---------------------------------------------------------------------------
#  MQTT mínimo (3.1.1)
# ---------------------------------------------------------------------------
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
            raise ConnectionError("el broker cerró la conexión")
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


class Mqtt:
    """Cliente mínimo. Si se corta la conexión con el broker, se reconecta solo, se vuelve a suscribir
    y deja en la cola el aviso RECONECTADO: lo que se publicó mientras no estaba se perdió."""
    RECONECTADO = ("", "reconectado")

    def __init__(self, host):
        self.host = host
        self.lock = threading.Lock()
        self.filtros = []
        self.mensajes = []
        self.hay = threading.Condition()
        self.sock = None
        self._conectar()
        threading.Thread(target=self._leer, daemon=True).start()
        threading.Thread(target=self._ping, daemon=True).start()

    def _conectar(self):
        sock = socket.create_connection((self.host, 1883), timeout=10)
        sock.sendall(paquete(0x10, cadena("MQTT") + b"\x04\x02\x00\x3c" + cadena(f"aura-ota-{os.getpid()}")))
        cab, cuerpo = leer_paquete(sock)
        if cab != 0x20 or cuerpo[1] != 0:
            sys.exit(f"El broker rechazó la conexión: {cuerpo!r}")
        sock.settimeout(None)
        with self.lock:
            self.sock = sock
            for f in self.filtros:
                sock.sendall(paquete(0x82, b"\x00\x01" + cadena(f) + b"\x00"))

    def _leer(self):
        while True:
            try:
                cab, cuerpo = leer_paquete(self.sock)
            except (ConnectionError, OSError):
                log("Se perdió la conexión con el broker: reintento")
                while True:
                    time.sleep(2)
                    try:
                        self._conectar()
                        break
                    except (ConnectionError, OSError):
                        pass
                log("Conectado otra vez al broker")
                with self.hay:
                    self.mensajes.append(self.RECONECTADO)
                    self.hay.notify()
                continue
            if cab >> 4 != 3:
                continue
            n = int.from_bytes(cuerpo[:2], "big")
            topico = cuerpo[2:2 + n].decode()
            i = 2 + n + (2 if (cab >> 1) & 3 else 0)
            with self.hay:
                self.mensajes.append((topico, cuerpo[i:].decode("utf-8", "replace")))
                self.hay.notify()

    def _enviar(self, datos):
        with self.lock:
            try:
                self.sock.sendall(datos)
                return True
            except (ConnectionError, OSError):
                return False

    def _ping(self):
        while True:
            time.sleep(30)
            self._enviar(b"\xc0\x00")

    def suscribir(self, filtro):
        self.filtros.append(filtro)
        self._enviar(paquete(0x82, b"\x00\x01" + cadena(filtro) + b"\x00"))

    def publicar(self, topico, payload):
        return self._enviar(paquete(0x30, cadena(topico) + payload.encode()))

    def siguiente(self, espera):
        with self.hay:
            if not self.mensajes:
                self.hay.wait(espera)
            return self.mensajes.pop(0) if self.mensajes else None


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Actualiza placas de la mesh de AURA por la malla")
    ap.add_argument("imagen")
    ap.add_argument("--hw", action="append", default=[], help="hw_id de una placa (se puede repetir)")
    ap.add_argument("--todas", action="store_true", help="todas las placas del tipo de la imagen")
    ap.add_argument("--aula", help="con --todas: solo las de esa aula")
    ap.add_argument("--broker", default="127.0.0.1")
    ap.add_argument("--ip", help="IP de esta PC que ve el gateway")
    ap.add_argument("--puerto-http", type=int, default=8070)
    ap.add_argument("--forzar-tipo", action="store_true")
    ap.add_argument("--esperar", type=float, default=1800)
    a = ap.parse_args()
    if bool(a.hw) == a.todas:
        sys.exit("Elegí las placas: --hw <hw_id> (una o varias) o --todas [--aula <aula>]")

    datos, tipo, version, chip = leer_imagen(a.imagen)
    sha = hashlib.sha256(datos).hexdigest()
    nombre = f"{tipo}-{version}.bin"
    ip = a.ip or mi_ip(a.broker)
    servir(datos, nombre, a.puerto_http)
    url = f"http://{ip}:{a.puerto_http}/{nombre}"
    log(f"Imagen {tipo} {version} ({chip}, {len(datos)} bytes, sha256 {sha[:16]}...)")
    log(f"Servida en {url}")

    m = Mqtt(a.broker)
    m.suscribir("hw/+/status")
    m.suscribir("hw/+/response")

    # Lo retenido: el último status de cada placa
    estado = {}
    fin = time.time() + 2
    while time.time() < fin:
        msg = m.siguiente(0.2)
        if msg and msg[0].endswith("/status"):
            try:
                estado[msg[0].split("/")[1]] = json.loads(msg[1])
            except ValueError:
                pass
    gateways = [h for h, s in estado.items() if (s.get("details") or {}).get("rol") == "raiz"]

    def fw_de(h):
        return (estado.get(h, {}).get("details") or {}).get("firmware") or {}

    if a.todas:
        placas, salteadas = [], []
        for h, s in sorted(estado.items()):
            det = s.get("details") or {}
            if fw_de(h).get("tipo") != tipo:
                continue
            if a.aula and (det.get("config") or {}).get("aula") != a.aula:
                continue
            (placas if s.get("status") == "online" else salteadas).append(h)
        for h in salteadas:
            log(f"{h}: offline, no se le pide (se la puede actualizar después con --hw)")
        if not placas:
            sys.exit(f"No hay placas online de tipo {tipo}" + (f" en {a.aula}" if a.aula else ""))
    else:
        placas = a.hw
        for h in placas:
            if not re.match(r"^mac-[0-9a-f]{12}$", h):
                sys.exit(f"hw_id inválido: {h}")
            f = fw_de(h)
            if f and f.get("tipo") != tipo and not a.forzar_tipo:
                log(f"Ojo: {h} es {f.get('tipo')} y la imagen es {tipo}: el gateway la va a rechazar (--forzar-tipo)")
    # El gateway, último en la lista (él igual lo deja para el final)
    placas.sort(key=lambda h: h in gateways)

    numero = [0]
    pedido = {}          # command_id -> hw_id
    situacion = {h: "pedida" for h in placas}
    detalle = {h: "" for h in placas}

    def pedir(h):
        numero[0] += 1
        cid = f"ota-{tipo}-{version}-{int(time.time())}-{numero[0]}"
        pedido[cid] = h
        params = {"url": url, "sha256": sha}
        if a.forzar_tipo:
            params["forzar_tipo"] = True
        m.publicar(f"hw/{h}/command", json.dumps({"command": "ota", "command_id": cid, "params": params}))

    log(f"Pido la actualización a {len(placas)} placa(s): {', '.join(placas)}")
    for h in placas:
        pedir(h)

    limite = time.time() + a.esperar
    uptime_gw = {g: (estado[g].get("details") or {}).get("uptime_s", 0) for g in gateways}
    while time.time() < limite and any(s not in ESTADOS_FINALES for s in situacion.values()):
        msg = m.siguiente(1)
        if not msg:
            continue
        if msg == Mqtt.RECONECTADO:
            # Las respuestas de mientras no estuvo se perdieron: se vuelve a pedir lo que no terminó
            pendientes = [x for x, s in situacion.items() if s in ("pedida", "transmitido", "recibido")]
            if pendientes:
                log(f"Vuelvo a pedir {', '.join(pendientes)} (lo de mientras no estaba se perdió)")
                for x in pendientes:
                    pedir(x)
            continue
        topico, payload = msg
        h = topico.split("/")[1]
        try:
            d = json.loads(payload)
        except ValueError:
            continue
        det = d.get("details") or {}
        if topico.endswith("/status"):
            estado[h] = d
            if h in uptime_gw or det.get("rol") == "raiz":
                ota = det.get("ota")
                if ota and "progreso" in ota:
                    log(f"gateway: {ota.get('actual')} {ota['progreso']} % (en cola {ota.get('en_cola')})")
                up = det.get("uptime_s")
                reinicio = d.get("status") == "offline" or (up is not None and up < uptime_gw.get(h, 0))
                if up is not None:
                    uptime_gw[h] = up
                if reinicio:
                    # "recibido" también: el gateway olvidó que esperaba su vuelta (si ya volvió con la
                    # versión nueva, el pedido repetido contesta aplicado sin_cambios)
                    pendientes = [x for x, s in situacion.items() if s in ("pedida", "transmitido", "recibido")]
                    if pendientes and d.get("status") == "online":
                        log(f"El gateway se reinició: vuelvo a pedir {', '.join(pendientes)}")
                        for x in pendientes:
                            pedir(x)
            continue
        cid = det.get("command_id")
        if cid not in pedido:
            continue
        h = pedido[cid]
        st = d.get("status")
        if situacion[h] in ESTADOS_FINALES:
            continue
        if st == "rechazado" and det.get("motivo") == "reemplazado_por_otro_pedido":
            continue
        situacion[h] = st
        fw = det.get("firmware") or {}
        texto = {
            "transmitido": "el gateway le ofreció la imagen",
            "recibido": "tiene la imagen verificada y se reinicia",
            "aplicado": (f"ya tenía {fw.get('tipo')} {fw.get('version')}" if det.get("sin_cambios")
                         else f"volvió con {fw.get('tipo')} {fw.get('version')}"),
            "rechazado": f"rechazada: {det.get('motivo')}" + (f" (tiene {fw.get('tipo')} {fw.get('version')})" if fw else ""),
        }.get(st, st)
        detalle[h] = texto
        log(f"{h}: {st} - {texto}")

    print()
    print("Resultado:")
    ok = True
    for h in placas:
        s = situacion[h]
        ok = ok and s == "aplicado"
        print(f"  {h}  {s:12s} {detalle[h] if s in ESTADOS_FINALES else 'sin terminar'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
