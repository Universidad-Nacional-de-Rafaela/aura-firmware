#!/usr/bin/env python3
"""Corre un conector en el banco, sin AURA, como lo va a correr AURA.

Lee los mensajes de una placa y los pasa por el conector con ContextoDePrueba (todo en
memoria). Muestra qué contestaría AURA a cada uno (persistido, descartado), qué
campos se quitaron, las alertas y las predicciones. No publica nada: el ack lo da AURA, o en el
banco herramientas/ack_falso.py.

Dos fuentes:

  - el broker: escucha hw/<hw_id>/data (y hw/<hw_id>/status para la configuración vigente)
        herramientas/correr_conector.py ejemplos/conector_ejemplo --hw-id mac-e072a1f7efe4 --docker aura-mosquitto

  - un archivo, una línea JSON por mensaje, con el payload de hw/<hw_id>/data:
        {"values": {"temp_chip_c": 35.2}, "ingest_id": "...", "ts": "2026-10-07T12:00:00Z"}
        herramientas/correr_conector.py ejemplos/conector_ejemplo --archivo mensajes.jsonl --predecir

Si el manifiesto tiene [prediccion], llama a predecir() cada "cada"; con --predecir, además,
una vez al final (útil con --archivo).

Con --subproceso, el conector corre como en AURA: cada mensaje en un proceso aparte, con
límites de memoria (256 MB), CPU (5 s) y archivos, y 10 s por mensaje (aura_sdk.subproceso).
Así se ve si algo que anda en el banco falla en AURA (un import pesado, un print() que se
esperaba ver, una muestra que tarda). Lo que imprime el conector va a stderr. predecir() no
corre en el subproceso: AURA todavía no lo llama.

Usa mosquitto_sub (del sistema o, con --docker, del contenedor del broker). Solo biblioteca estándar.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "comun" / "python"))
from aura_sdk import (  # noqa: E402
    ConectorEnSubproceso, ContextoDePrueba, ManifiestoInvalido, Mensaje, cargar_conector, cargar_manifiesto, procesar,
)

SDK_DIR = Path(__file__).resolve().parent.parent / "comun" / "python"


class Corrida:
    """Estado de una corrida: el conector, su contexto y lo ya mostrado."""

    def __init__(self, carpeta, hw_id, salida=print, reloj=time.monotonic, subproceso=False):
        self.subproceso = subproceso
        if subproceso:   # como en AURA: el manifiesto acá, la clase recién en el hijo
            self.manifiesto = cargar_manifiesto(carpeta)
            self.conector = ConectorEnSubproceso(carpeta, SDK_DIR)
        else:
            self.manifiesto, self.conector = cargar_conector(carpeta)
        self.ctx = ContextoDePrueba(self.manifiesto)
        self.hw_id = hw_id
        self.salida = salida
        self.reloj = reloj
        self.ultima_prediccion = reloj()
        self.conteo = {}

    def data(self, payload):
        try:
            msg = Mensaje.desde_data(self.hw_id, payload)
        except ValueError as e:
            self.salida(f"  !! {e}: AURA contestaría rechazado (values_invalido)")
            self._contar("rechazado")
            return None
        alertas_antes = len(self.ctx.alertas)
        r = procesar(self.conector, self.ctx, msg)
        if self.subproceso and self.conector.stderr:   # prints y log del hijo: a stderr, como en el banco
            sys.stderr.write(self.conector.stderr if self.conector.stderr.endswith("\n") else self.conector.stderr + "\n")
            sys.stderr.flush()
        extra = f" ({r.motivo})" if r.motivo else ""
        quitados = f", quitados {r.quitados}" if r.quitados else ""
        self.salida(f"{msg.values} -> {r.resultado}{extra}{quitados}")
        if r.error:
            self.salida("   " + r.error.strip().splitlines()[-1])
        for a in self.ctx.alertas[alertas_antes:]:
            self.salida(f"   alerta {a.tipo} [{a.severidad}] {a.mensaje}")
        self._contar(r.resultado)
        self.quizas_predecir()
        return r

    def status(self, payload):
        config = payload.get("details", {}).get("config") if isinstance(payload, dict) else None
        if isinstance(config, dict):
            self.ctx._config = dict(config)
            self.salida(f"   config vigente del dispositivo: {config}")

    def quizas_predecir(self, forzar=False):
        p = self.manifiesto.prediccion
        if not forzar and (p is None or self.reloj() - self.ultima_prediccion < p.cada_s):
            return
        self.ultima_prediccion = self.reloj()
        if self.subproceso:
            self.salida("   predecir() no corre en el subproceso: AURA todavía no lo llama")
            return
        antes = len(self.ctx.predicciones)
        try:
            self.conector.predecir(self.ctx)
        except Exception as e:   # en AURA, predecir() que falla no afecta la ingesta
            self.salida(f"   !! predecir() falló: {e!r}")
            return
        for pr in self.ctx.predicciones[antes:]:
            self.salida(f"   predicción {pr.campo} para {pr.ts_objetivo:%Y-%m-%d %H:%M} UTC: {pr.valor} "
                        f"[{pr.inferior}, {pr.superior}] ({pr.modelo})")
        if len(self.ctx.predicciones) == antes:
            self.salida("   predecir() no guardó ninguna predicción")

    def resumen(self):
        partes = ", ".join(f"{k}: {v}" for k, v in sorted(self.conteo.items())) or "ningún mensaje"
        self.salida(f"--- {partes}; {len(self.ctx.mediciones)} mediciones guardadas, "
                    f"{len(self.ctx.alertas)} alertas, {len(self.ctx.predicciones)} predicciones")

    def _contar(self, clave):
        self.conteo[clave] = self.conteo.get(clave, 0) + 1


def desde_archivo(corrida, archivo):
    for n, linea in enumerate(Path(archivo).read_text(encoding="utf-8").splitlines(), 1):
        if not linea.strip():
            continue
        try:
            corrida.data(json.loads(linea))
        except json.JSONDecodeError:
            corrida.salida(f"línea {n}: no es JSON, se saltea")


def desde_broker(corrida, args):
    base = ["docker", "exec", "-i", args.docker, "mosquitto_sub"] if args.docker else ["mosquitto_sub"]
    comando = base + ["-h", args.host, "-p", str(args.puerto), "-v", "-q", "1",
                      "-t", f"hw/{args.hw_id}/data", "-t", f"hw/{args.hw_id}/status"]
    sub = subprocess.Popen(comando, stdout=subprocess.PIPE, text=True)
    corrida.salida(f"escuchando hw/{args.hw_id}/data en {args.host}:{args.puerto} (Ctrl+C para terminar)")
    try:
        for linea in sub.stdout:
            topico, _, cuerpo = linea.rstrip("\n").partition(" ")
            try:
                payload = json.loads(cuerpo)
            except json.JSONDecodeError:
                corrida.salida(f"{topico}: no es JSON")
                continue
            if topico.endswith("/status"):
                corrida.status(payload)
            else:
                corrida.data(payload)
    except KeyboardInterrupt:
        pass
    finally:
        sub.terminate()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("carpeta", help="carpeta del conector (conectores/<CÓDIGO> o ejemplos/conector_ejemplo)")
    ap.add_argument("--hw-id", default="mac-000000000000", help="la placa (mac-…); obligatorio con el broker")
    ap.add_argument("--archivo", help="mensajes de un archivo, uno por línea, en vez del broker")
    ap.add_argument("--predecir", action="store_true", help="llamar a predecir() al final")
    ap.add_argument("--subproceso", action="store_true",
                    help="correr el conector como en AURA: en un proceso aparte, con límites")
    ap.add_argument("--config", help='configuración vigente, en JSON: \'{"intervalo_s": 60}\'')
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--puerto", type=int, default=1883)
    ap.add_argument("--docker", help="contenedor del broker (por ejemplo aura-mosquitto)")
    args = ap.parse_args()

    try:
        corrida = Corrida(args.carpeta, args.hw_id, subproceso=args.subproceso)
    except ManifiestoInvalido as e:
        sys.exit(f"el conector no carga: {e}")
    if args.config:
        corrida.ctx._config = json.loads(args.config)
    m = corrida.manifiesto
    print(f"conector {m.nombre} {m.version}, campos {', '.join(m.campos)}"
          + (f", predice cada {m.prediccion.cada_s // 60} min" if m.prediccion else ""))

    if args.archivo:
        desde_archivo(corrida, args.archivo)
    else:
        if args.hw_id == "mac-000000000000":
            sys.exit("con el broker hace falta --hw-id mac-<MAC de la placa> (herramientas/leer_mac)")
        desde_broker(corrida, args)
    if args.predecir:
        corrida.quizas_predecir(forzar=True)
    corrida.resumen()


if __name__ == "__main__":
    main()
