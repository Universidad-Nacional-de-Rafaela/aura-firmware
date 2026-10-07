#!/usr/bin/env python3
"""Corre un conector en el banco, sin AURA, como lo va a correr AURA.

Lee los mensajes de una placa y los pasa por el conector con ContextoDePrueba (todo en
memoria). Muestra qué contestaría AURA a cada uno (persistido, descartado, sin ack), qué
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

Usa mosquitto_sub (del sistema o, con --docker, del contenedor del broker). Solo biblioteca estándar.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "comun" / "python"))
from aura_sdk import ContextoDePrueba, ManifiestoInvalido, Mensaje, cargar_conector, procesar  # noqa: E402


class Corrida:
    """Estado de una corrida: el conector, su contexto y lo ya mostrado."""

    def __init__(self, carpeta, hw_id, salida=print, reloj=time.monotonic):
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
        if r.resultado is None:
            self.salida(f"{msg.values} -> SIN ACK: el conector lanzó una excepción (el nodo reenviaría)")
            self.salida("   " + r.error.strip().splitlines()[-1])
        else:
            extra = f" ({r.motivo})" if r.motivo else ""
            quitados = f", quitados {r.quitados}" if r.quitados else ""
            self.salida(f"{msg.values} -> {r.resultado}{extra}{quitados}")
        for a in self.ctx.alertas[alertas_antes:]:
            self.salida(f"   alerta {a.tipo} [{a.severidad}] {a.mensaje}")
        self._contar(r.resultado or "sin_ack")
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
    ap.add_argument("--config", help='configuración vigente, en JSON: \'{"intervalo_s": 60}\'')
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--puerto", type=int, default=1883)
    ap.add_argument("--docker", help="contenedor del broker (por ejemplo aura-mosquitto)")
    args = ap.parse_args()

    try:
        corrida = Corrida(args.carpeta, args.hw_id)
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
