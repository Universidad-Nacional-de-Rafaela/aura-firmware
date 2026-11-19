#!/usr/bin/env python3
"""Cliente de la estacion de enrolamiento de NTAG 424 DNA (enrolador_ntag424.ino).

Solo biblioteca estandar. Habla por el puerto serie con la placa de la estacion (XIAO C3 + RC522).

    enrolar.py info
    enrolar.py uid                     # dice el UID de la tarjeta apoyada
    enrolar.py estado                  # que clave tiene (no cambia nada)
    enrolar.py enrolar                 # cambia la clave de fabrica por la derivada del UID
    enrolar.py desenrolar              # la devuelve a la clave de fabrica
    enrolar.py lote                    # enrola tarjeta tras tarjeta hasta que cortes con Ctrl+C
    enrolar.py auto [on|off]           # modo autonomo (boton + buzzer), ver abajo
    enrolar.py maestra ARCHIVO         # (solo modo banco) carga la maestra; el archivo tiene 64 hexa

Modo autonomo (sin PC): con el boton suelto, apoyar una tarjeta la enrola; con el boton apretado
desde antes, la borra. Despues de cualquier orden por USB el autonomo espera 30 s (`auto on` lo
reactiva ya) para no pisar a esta herramienta.

Las ordenes que modifican primero leen el UID, te lo muestran y piden confirmacion (--si la saltea);
despues le piden a la estacion que actue SOLO sobre esa tarjeta. La clave maestra nunca viaja en la
linea de comandos (queda en el historial del shell): se lee de un archivo.
"""
import argparse
import glob
import os
import select
import sys
import termios
import time

RESPUESTA_MAX_S = 20


def buscar_puerto():
    for patron in ("/dev/serial/by-id/*Espressif*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        hallados = sorted(glob.glob(patron))
        if len(hallados) == 1:
            return hallados[0]
        if len(hallados) > 1:
            sys.exit("Hay varios puertos (%s): elegi uno con --puerto" % ", ".join(hallados))
    sys.exit("No encuentro la estacion: conectala o pasa --puerto")


class Estacion:
    def __init__(self, puerto):
        self.fd = os.open(puerto, os.O_RDWR | os.O_NOCTTY)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0                     # raw
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[4] = a[5] = termios.B115200
        a[6][termios.VMIN], a[6][termios.VTIME] = 0, 0
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        self.buf = b""
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def cerrar(self):
        os.close(self.fd)

    def _linea(self, limite):
        while b"\n" not in self.buf:
            falta = limite - time.time()
            if falta <= 0:
                return None
            listo, _, _ = select.select([self.fd], [], [], falta)
            if listo:
                self.buf += os.read(self.fd, 256)
        linea, self.buf = self.buf.split(b"\n", 1)
        return linea.decode(errors="replace").strip()

    def orden(self, texto):
        """Manda una orden y devuelve la primera linea que empieza con OK o ERR."""
        termios.tcflush(self.fd, termios.TCIFLUSH)
        self.buf = b""
        os.write(self.fd, (texto + "\n").encode())
        limite = time.time() + RESPUESTA_MAX_S
        while True:
            linea = self._linea(limite)
            if linea is None:
                return "ERR sin_respuesta_de_la_estacion"
            if linea.startswith(("OK", "ERR")):
                return linea


def leer_uid(est):
    print("Apoya la tarjeta en la estacion...")
    r = est.orden("uid")
    if not r.startswith("OK uid "):
        sys.exit("No pude leer la tarjeta: " + r)
    return r.split()[2]


def confirmar(pregunta, sin_preguntar):
    if sin_preguntar:
        return True
    return input(pregunta + " [s/N] ").strip().lower() in ("s", "si", "y")


def sobre_tarjeta(est, que, args):
    uid = leer_uid(est)
    print("Tarjeta:", uid)
    if que != "estado" and not confirmar("%s la tarjeta %s?" % (que.capitalize(), uid), args.si):
        print("Cancelado.")
        return 1
    r = est.orden("%s %s" % (que, uid))
    print(r)
    return 0 if r.startswith("OK") else 1


def lote(est, args):
    print("Enrolando una tras otra. Ctrl+C para cortar.")
    hechas = 0
    fallas = 0
    try:
        while True:
            input("Apoya la proxima tarjeta y toca Enter... ")
            if sobre_tarjeta(est, "enrolar", args) == 0:
                hechas += 1
            else:
                fallas += 1
    except (KeyboardInterrupt, EOFError):
        print("\nEnroladas: %d, con problemas: %d" % (hechas, fallas))
    return 0 if fallas == 0 else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("accion", choices=["info", "uid", "estado", "enrolar", "desenrolar", "lote", "maestra", "auto"])
    p.add_argument("archivo", nargs="?", help="para 'maestra': archivo con 64 hexa; para 'auto': on u off")
    p.add_argument("--puerto", help="puerto serie de la estacion (por defecto, el unico que haya)")
    p.add_argument("--si", action="store_true", help="no pedir confirmacion")
    args = p.parse_args()

    est = Estacion(args.puerto or buscar_puerto())
    try:
        if args.accion == "info":
            r = est.orden("info")
            print(r)
            return 0 if r.startswith("OK") else 1
        if args.accion == "uid":
            print(leer_uid(est))
            return 0
        if args.accion in ("estado", "enrolar", "desenrolar"):
            return sobre_tarjeta(est, args.accion, args)
        if args.accion == "lote":
            return lote(est, args)
        if args.accion == "auto":
            r = est.orden("auto" + (" " + args.archivo if args.archivo else ""))
            print(r)
            return 0 if r.startswith("OK") else 1
        if args.accion == "maestra":
            if not args.archivo:
                sys.exit("Falta el archivo con la maestra")
            clave = open(args.archivo).read().strip()
            if len(clave) != 64 or any(c not in "0123456789abcdefABCDEF" for c in clave):
                sys.exit("La maestra tiene que ser 64 caracteres hexa")
            r = est.orden("maestra " + clave)
            print(r)
            return 0 if r.startswith("OK") else 1
    finally:
        est.cerrar()


if __name__ == "__main__":
    sys.exit(main())
