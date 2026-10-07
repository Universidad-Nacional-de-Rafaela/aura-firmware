#!/usr/bin/env python3
"""AURA falsa para probar la mesh en banco sin el backend: contesta los ack.

Escucha hw/+/data en el broker y, por cada mensaje con ingest_id, publica en
hw/<hw_id>/ack lo mismo que publicaría AURA después de guardar (contrato v4.0,
§3.6). Con eso el raíz le confirma la muestra a la hoja, y la hoja la saca de
su cola. Deduplica por ingest_id: un reintento sale "duplicado".

Para ejercitar los caminos de error, el modo se cambia en caliente escribiendo
su nombre y Enter:

    ok          normal: persistido, o duplicado si ya lo vio
    sin_ack     base caída: no contesta nada (la hoja reintenta 15 s ... 5 min)
    rechazado   contesta rechazado (la hoja libera la muestra igual)
    cuarentena  placa sin asignar: cuarentena (la hoja la libera)

Usa mosquitto_sub y mosquitto_pub: los del sistema o, con --docker, los del
contenedor del broker (no hace falta instalar nada). Solo biblioteca estándar.

Uso: herramientas/ack_falso.py [--host localhost] [--puerto 1883] [--docker aura-mosquitto]
"""

import argparse
import json
import re
import subprocess
import sys
import threading
from datetime import datetime

MODOS = ("ok", "sin_ack", "rechazado", "cuarentena")
_TOPICO_DATA = re.compile(r"^hw/((?:mac-[0-9a-f]{12})|(?:eui-[0-9a-f]{16})|(?:svc-[a-z0-9-]{1,32}))/data$")


def hw_id_del_topico(topico):
    m = _TOPICO_DATA.match(topico or "")
    return m.group(1) if m else None


def decidir(payload, modo, vistos):
    """El ack que publicaría AURA para un data, o None si no publicaría nada.

    vistos es el conjunto de ingest_id ya "guardados"; se actualiza acá.
    """
    ingest_id = payload.get("ingest_id")
    if not isinstance(ingest_id, str) or not ingest_id:
        return None   # sin ingest_id AURA guarda pero no confirma (no hay qué confirmar)
    if modo == "sin_ack":
        return None
    if not isinstance(payload.get("values"), dict):
        return {"ingest_id": ingest_id, "resultado": "rechazado", "motivo": "values_invalido"}
    if modo == "rechazado":
        return {"ingest_id": ingest_id, "resultado": "rechazado", "motivo": "modo_rechazado"}
    if ingest_id in vistos:
        return {"ingest_id": ingest_id, "resultado": "duplicado"}
    vistos.add(ingest_id)
    return {"ingest_id": ingest_id, "resultado": "cuarentena" if modo == "cuarentena" else "persistido"}


def _cliente(nombre, args):
    base = ["docker", "exec", "-i", args.docker, nombre] if args.docker else [nombre]
    return base + ["-h", args.host, "-p", str(args.puerto)]


def _leer_modos(estado):
    for linea in sys.stdin:
        modo = linea.strip()
        if modo in MODOS:
            estado["modo"] = modo
            print(f"--- modo: {modo}", flush=True)
        elif modo:
            print(f"--- modos: {', '.join(MODOS)}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--puerto", type=int, default=1883)
    ap.add_argument("--docker", help="contenedor del broker (por ejemplo aura-mosquitto)")
    args = ap.parse_args()

    estado = {"modo": "ok"}
    vistos = set()
    threading.Thread(target=_leer_modos, args=(estado,), daemon=True).start()

    sub = subprocess.Popen(_cliente("mosquitto_sub", args) + ["-t", "hw/+/data", "-v", "-q", "1"],
                           stdout=subprocess.PIPE, text=True)
    print(f"ack_falso escuchando hw/+/data en {args.host}:{args.puerto}, modo ok "
          f"(escribir {'/'.join(MODOS)} + Enter para cambiar)", flush=True)
    try:
        for linea in sub.stdout:
            topico, _, cuerpo = linea.rstrip("\n").partition(" ")
            hw_id = hw_id_del_topico(topico)
            if not hw_id:
                continue
            try:
                payload = json.loads(cuerpo)
            except json.JSONDecodeError:
                print(f"{datetime.now():%H:%M:%S} {hw_id} !! no es JSON, sin ack: {cuerpo}", flush=True)
                continue
            if not isinstance(payload, dict):
                continue
            ack = decidir(payload, estado["modo"], vistos)
            hora = f"{datetime.now():%H:%M:%S}"
            if ack is None:
                print(f"{hora} {hw_id} {payload.get('ingest_id')} -> sin ack ({estado['modo']})", flush=True)
                continue
            subprocess.run(_cliente("mosquitto_pub", args) +
                           ["-t", f"hw/{hw_id}/ack", "-q", "1", "-m", json.dumps(ack)], check=False)
            print(f"{hora} {hw_id} {payload.get('values')} -> {ack['resultado']} "
                  f"({len(vistos)} guardadas)", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        sub.terminate()


if __name__ == "__main__":
    main()
