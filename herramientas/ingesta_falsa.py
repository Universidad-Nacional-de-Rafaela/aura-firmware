#!/usr/bin/env python3
"""API de ingesta falsa para probar la mesh en banco sin el backend de AURA.

Atiende POST /api/v1/telemetry/ingest con el mismo contrato que el real
(docs/CONTRATO_MQTT.md §6): 201 con {"inserted", "duplicates", "errors", "message"},
y deduplicación por ingest_id. Solo usa la biblioteca estándar.

Para ejercitar los caminos de error, el modo se cambia en caliente:

    curl localhost:8000/_modo/ok        # normal
    curl localhost:8000/_modo/500       # backend caído: HTTP 500
    curl localhost:8000/_modo/errores   # el defecto real: 201 con errors=1 y nada guardado
    curl localhost:8000/_resumen        # cuántos eventos, cuántos ingest_id únicos, por dispositivo

Con --archivo, los ingest_id se guardan en disco y sobreviven a reiniciar este
programa (para la prueba de "el backend se reinicia en el medio").

Uso: herramientas/ingesta_falsa.py [--puerto 8000] [--archivo ingest.json]
"""

import argparse
import json
import sys
import threading
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CAMPOS = ("tenant_id", "device_id", "type", "ingest_id", "payload")
MODOS = ("ok", "500", "errores")


class Estado:
    def __init__(self, archivo):
        self.archivo = archivo
        self.modo = "ok"
        self.lock = threading.Lock()
        self.eventos = {}  # ingest_id -> evento
        self.recibidos = 0
        self.duplicados = 0
        if archivo:
            try:
                with open(archivo, encoding="utf-8") as f:
                    self.eventos = json.load(f)
            except FileNotFoundError:
                pass

    def guardar(self):
        if self.archivo:
            with open(self.archivo, "w", encoding="utf-8") as f:
                json.dump(self.eventos, f, indent=1)

    def resumen(self):
        por_dispositivo = {}
        for ev in self.eventos.values():
            por_dispositivo[ev["device_id"]] = por_dispositivo.get(ev["device_id"], 0) + 1
        return {
            "modo": self.modo,
            "eventos_recibidos": self.recibidos,
            "ingest_id_unicos": len(self.eventos),
            "duplicados": self.duplicados,
            "por_dispositivo": por_dispositivo,
        }


def hora():
    return datetime.now().strftime("%H:%M:%S")


class Manejador(BaseHTTPRequestHandler):
    estado: Estado = None

    def responder(self, codigo, cuerpo):
        datos = json.dumps(cuerpo).encode()
        self.send_response(codigo)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(datos)))
        self.end_headers()
        self.wfile.write(datos)

    def do_GET(self):
        e = self.estado
        if self.path.startswith("/_modo/"):
            modo = self.path[len("/_modo/"):]
            if modo not in MODOS:
                self.responder(400, {"error": f"modos: {', '.join(MODOS)}"})
                return
            e.modo = modo
            print(f"{hora()} == modo: {modo}", flush=True)
            self.responder(200, {"modo": modo})
        elif self.path == "/_resumen":
            with e.lock:
                self.responder(200, e.resumen())
        else:
            self.responder(404, {"error": "no existe"})

    def do_POST(self):
        e = self.estado
        if self.path != "/api/v1/telemetry/ingest":
            self.responder(404, {"error": "no existe"})
            return
        largo = int(self.headers.get("Content-Length", 0))
        try:
            cuerpo = json.loads(self.rfile.read(largo))
            eventos = cuerpo["events"]
            assert isinstance(eventos, list)
        except Exception:
            self.responder(422, {"detail": "cuerpo inválido"})
            return

        if e.modo == "500":
            print(f"{hora()} !! 500 a {len(eventos)} evento(s)", flush=True)
            self.responder(500, {"detail": "backend caído (simulado)"})
            return

        inserted = duplicates = errors = 0
        with e.lock:
            for ev in eventos:
                e.recibidos += 1
                faltan = [c for c in CAMPOS if c not in ev]
                if faltan or e.modo == "errores" or not isinstance(ev.get("payload"), dict):
                    errors += 1
                    print(f"{hora()} !! error {ev.get('ingest_id')} faltan={faltan}", flush=True)
                    continue
                if ev["ingest_id"] in e.eventos:
                    duplicates += 1
                    e.duplicados += 1
                    print(f"{hora()} == duplicado {ev['ingest_id']}", flush=True)
                    continue
                e.eventos[ev["ingest_id"]] = ev
                inserted += 1
                print(f"{hora()} OK {ev['device_id']} {ev['type']} ts={ev.get('ts', '-')} "
                      f"{json.dumps(ev['payload'])} [{ev['ingest_id']}]", flush=True)
            e.guardar()

        # Como el real: 201 aunque haya errores (contrato §6, defecto 2).
        self.responder(201, {"inserted": inserted, "duplicates": duplicates, "errors": errors,
                             "message": "simulado"})

    def log_message(self, *args):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--puerto", type=int, default=8000)
    ap.add_argument("--archivo", help="guardar los ingest_id en este JSON")
    args = ap.parse_args()

    Manejador.estado = Estado(args.archivo)
    servidor = ThreadingHTTPServer(("0.0.0.0", args.puerto), Manejador)
    print(f"ingesta falsa en :{args.puerto} (modo ok); /_modo/<{'|'.join(MODOS)}>, /_resumen",
          flush=True)
    try:
        servidor.serve_forever()
    except KeyboardInterrupt:
        print(json.dumps(Manejador.estado.resumen(), indent=1))
        sys.exit(0)


if __name__ == "__main__":
    main()
