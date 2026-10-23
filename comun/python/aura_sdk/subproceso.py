"""El conector en un proceso aparte: el runner del hijo, el padre y ConectorEnSubproceso.

AURA no importa el código de un grupo en su proceso: lo corre en un hijo sin credenciales, con
límites de memoria, CPU y archivos, y un timeout de reloj por muestra. Hablan por stdin/stdout,
una línea JSON por mensaje:

    padre -> hijo  {"tipo": "inicio", "carpeta": "…", "mensaje": {"hw_id", "values", "ts", "ingest_id", "adaptador"}}
    hijo -> padre  {"tipo": "llamada", "metodo": "serie", "args": {…}}
    padre -> hijo  {"tipo": "respuesta", "ok": true, "valor": …}
                   {"tipo": "respuesta", "ok": false, "error": "KeyError", "detalle": "…"}
    hijo -> padre  {"tipo": "fin", "ok": true}  o  {"tipo": "fin", "ok": false, "error": "<traceback>"}

Las fechas viajan como {"$ts": "<ISO 8601 UTC>"}. Un error del Contexto que es KeyError,
ValueError, TypeError o NotImplementedError se relanza en el hijo con el mismo tipo (cualquier
otro nombre llega como RuntimeError): el conector ve lo mismo que con ContextoDePrueba.

ConectorEnSubproceso se usa como cualquier conector dentro de procesar(): si el hijo falla,
lanza FalloDelConector con la causa, y procesar() aplica la D5. Si falla el Contexto con otro
tipo de error (la base no contesta), mata al hijo y lanza ErrorDelAnfitrion: es de AURA.

El hijo se corre así (lo arma ConectorEnSubproceso):
    python -I <sdk_dir>/aura_sdk/subproceso.py --mem-mb 256 --cpu-s 5 --archivos 64
"""
import sys
from pathlib import Path

if __name__ == "__main__" and not __package__:
    # Corrido como script: el SDK es la carpeta de arriba (con -I no se agrega sola).
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from aura_sdk.subproceso import main
    sys.exit(main())

import argparse
import json
import logging
import os
import selectors
import signal
import subprocess
import time
import traceback
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Optional

from .cargar import cargar_conector
from .contexto import Contexto
from .ejecutar import ErrorDelAnfitrion, FalloDelConector
from .mensaje import Mensaje

METODOS = ("guardar_medicion", "descartar", "serie", "alertar", "config_vigente", "guardar_prediccion")
ERRORES = {e.__name__: e for e in (KeyError, ValueError, TypeError, NotImplementedError)}
MAX_STDERR = 4096                  # lo que se guarda del stderr del hijo (el final)
MAX_LINEA = 4 * 1024 * 1024        # una línea del hijo más larga que esto es un error de protocolo

__all__ = ["ConectorEnSubproceso", "ErrorDelAnfitrion", "Limites", "codificar", "decodificar", "hijo", "main"]


@dataclass(frozen=True)
class Limites:
    mem_mb: int = 256      # RLIMIT_AS
    cpu_s: int = 5         # RLIMIT_CPU
    archivos: int = 64     # RLIMIT_NOFILE
    procesos: Optional[int] = None   # RLIMIT_NPROC (cuenta todos los procesos del usuario; None: no se pone)


# ----- protocolo -----

def _a_json(obj):
    if isinstance(obj, datetime):
        t = obj if obj.tzinfo else obj.replace(tzinfo=timezone.utc)
        return {"$ts": t.astimezone(timezone.utc).isoformat()}
    raise TypeError(f"{type(obj).__name__} no se puede mandar al otro proceso")


def _de_json(d):
    if len(d) == 1 and isinstance(d.get("$ts"), str):
        return datetime.fromisoformat(d["$ts"])
    return d


def codificar(obj) -> str:
    """Una línea JSON (sin el \\n). Las fechas van como {"$ts": "<ISO 8601 UTC>"}; sin zona, son UTC."""
    return json.dumps(obj, default=_a_json, ensure_ascii=True)


def decodificar(linea: str) -> dict:
    """La línea como dict, con las fechas como datetime. ValueError si no es un objeto JSON."""
    obj = json.loads(linea, object_hook=_de_json)
    if not isinstance(obj, dict):
        raise ValueError("la línea no es un objeto JSON")
    return obj


# ----- el hijo -----

def poner_limites(limites: Limites) -> None:
    """soft = hard: el conector no los puede volver a subir. Si el hard actual es menor, queda ese."""
    import resource
    pedidos = [(resource.RLIMIT_AS, limites.mem_mb * 1024 * 1024),
               (resource.RLIMIT_CPU, limites.cpu_s),
               (resource.RLIMIT_NOFILE, limites.archivos)]
    if limites.procesos is not None:
        pedidos.append((resource.RLIMIT_NPROC, limites.procesos))
    for recurso, valor in pedidos:
        _, duro = resource.getrlimit(recurso)
        if duro != resource.RLIM_INFINITY:
            valor = min(valor, duro)
        resource.setrlimit(recurso, (valor, valor))


class ContextoProxy(Contexto):
    """El Contexto del lado del hijo: cada método es una llamada al padre."""

    def __init__(self, entrada, enviar, manifiesto):
        self._entrada = entrada
        self._enviar = enviar
        self.manifiesto = manifiesto
        self.log = logging.getLogger(f"conector.{manifiesto.nombre}")

    def _llamar(self, metodo, **args):
        self._enviar({"tipo": "llamada", "metodo": metodo, "args": args})
        linea = self._entrada.readline()
        if not linea:
            raise RuntimeError("AURA cerró la conexión")
        r = decodificar(linea)
        if r.get("tipo") != "respuesta":
            raise RuntimeError(f"se esperaba una respuesta y llegó {r.get('tipo')!r}")
        if r.get("ok"):
            return r.get("valor")
        raise ERRORES.get(r.get("error"), RuntimeError)(r.get("detalle", ""))

    def guardar_medicion(self, values, ts=None):
        return self._llamar("guardar_medicion", values=values, ts=ts)

    def descartar(self, motivo):
        self._llamar("descartar", motivo=motivo)

    def serie(self, campo, desde=None, hasta=None):
        return [tuple(p) for p in self._llamar("serie", campo=campo, desde=desde, hasta=hasta)]

    def guardar_prediccion(self, campo, ts_objetivo, valor, inferior=None, superior=None, modelo=""):
        self._llamar("guardar_prediccion", campo=campo, ts_objetivo=ts_objetivo, valor=valor,
                     inferior=inferior, superior=superior, modelo=modelo)

    def alertar(self, tipo, severidad, mensaje):
        self._llamar("alertar", tipo=tipo, severidad=severidad, mensaje=mensaje)

    def config_vigente(self):
        return self._llamar("config_vigente")


def hijo(entrada, salida, limites: Optional[Limites]) -> int:
    """Corre una muestra: lee el inicio, carga el conector y llama a al_recibir_datos.

    entrada y salida son archivos de texto (en el proceso hijo, stdin y el stdout real). Pone los
    límites antes de importar el conector (None: no los pone, para tests). Devuelve 0 si el
    conector terminó bien y 1 si no; en los dos casos el último mensaje es el fin."""
    def enviar(obj):
        salida.write(codificar(obj) + "\n")
        salida.flush()

    try:
        if limites is not None:
            poner_limites(limites)
        inicio = decodificar(entrada.readline())
        if inicio.get("tipo") != "inicio":
            raise RuntimeError(f"se esperaba el inicio y llegó {inicio.get('tipo')!r}")
        m = inicio["mensaje"]
        manifiesto, conector = cargar_conector(inicio["carpeta"])
        msg = Mensaje(hw_id=m["hw_id"], values=m["values"], ts=m.get("ts"),
                      ingest_id=m.get("ingest_id"), adaptador=m.get("adaptador"))
        conector.al_recibir_datos(msg, ContextoProxy(entrada, enviar, manifiesto))
    except BaseException:   # también SystemExit: un sys.exit() del conector es una falla
        enviar({"tipo": "fin", "ok": False, "error": traceback.format_exc()})
        return 1
    enviar({"tipo": "fin", "ok": True})
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Runner del hijo: corre un conector, hablando por stdin/stdout.")
    ap.add_argument("--mem-mb", type=int, default=Limites.mem_mb)
    ap.add_argument("--cpu-s", type=int, default=Limites.cpu_s)
    ap.add_argument("--archivos", type=int, default=Limites.archivos)
    ap.add_argument("--procesos", type=int, default=None)
    args = ap.parse_args(argv)
    # El stdout real es del protocolo: un print() del conector va a stderr.
    salida = sys.stdout
    sys.stdout = sys.stderr
    logging.basicConfig(stream=sys.stderr, level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")
    return hijo(sys.stdin, salida, Limites(args.mem_mb, args.cpu_s, args.archivos, args.procesos))


# ----- el padre -----

class _Vencido(Exception):
    pass


class _Sesion:
    """Una muestra: el hijo, su stdout con un buffer, el final de su stderr y la fecha límite.

    stdout y stderr se leen en el mismo bucle: el hijo nunca se traba escribiendo en stderr, y del
    stderr solo se guardan los últimos MAX_STDERR bytes (nada va a disco)."""

    def __init__(self, proc, limite):
        self.proc = proc
        self.limite = limite
        self.buffer = bytearray()
        self.cola_stderr = bytearray()
        self.stdout_cerrado = False
        for f in (proc.stdin, proc.stdout, proc.stderr):
            os.set_blocking(f.fileno(), False)
        self.selector = selectors.DefaultSelector()
        self.selector.register(proc.stdout, selectors.EVENT_READ)
        self.selector.register(proc.stderr, selectors.EVENT_READ)

    def _restante(self):
        restante = self.limite - time.monotonic()
        if restante <= 0:
            raise _Vencido
        return restante

    def _leer_stderr(self):
        """Lee lo que haya en stderr. False si se cerró."""
        datos = os.read(self.proc.stderr.fileno(), 65536)
        if not datos:
            return False
        self.cola_stderr += datos
        del self.cola_stderr[:-MAX_STDERR]
        return True

    def _esperar(self, quiero):
        """Espera hasta que stdin (quiero=proc.stdin) o stdout esté listo, atendiendo stderr mientras tanto."""
        while True:
            for clave, _ in self.selector.select(self._restante()):
                if clave.fileobj is self.proc.stderr:
                    self._atender_stderr()
                elif clave.fileobj is quiero:
                    return

    def _atender_stderr(self):
        try:
            abierto = self._leer_stderr()
        except BlockingIOError:
            return
        if not abierto:
            self.selector.unregister(self.proc.stderr)

    def esperar_fin(self):
        """Espera a que el hijo termine, atendiendo stderr. _Vencido si no termina a tiempo."""
        while self.proc.poll() is None:
            for clave, _ in self.selector.select(min(self._restante(), 0.05)):
                if clave.fileobj is self.proc.stderr:
                    self._atender_stderr()

    def enviar(self, linea: str):
        """Escribe una línea sin bloquear más allá de la fecha límite. False si el hijo cerró su stdin."""
        datos = memoryview((linea + "\n").encode("ascii"))
        fd = self.proc.stdin.fileno()
        self.selector.register(self.proc.stdin, selectors.EVENT_WRITE)
        try:
            while datos:
                self._esperar(self.proc.stdin)
                try:
                    n = os.write(fd, datos)
                except BlockingIOError:
                    continue
                except BrokenPipeError:
                    return False
                datos = datos[n:]
            return True
        finally:
            self.selector.unregister(self.proc.stdin)

    def linea(self):
        """La próxima línea del hijo (bytes, sin el \\n), o None si cerró su stdout."""
        fd = self.proc.stdout.fileno()
        while b"\n" not in self.buffer:
            if len(self.buffer) > MAX_LINEA:
                raise FalloDelConector("protocolo", f"una línea de más de {MAX_LINEA} bytes")
            if self.stdout_cerrado:
                return None
            self._esperar(self.proc.stdout)
            try:
                datos = os.read(fd, 65536)
            except BlockingIOError:
                continue
            if not datos:
                self.stdout_cerrado = True
                self.selector.unregister(self.proc.stdout)
                return None
            self.buffer += datos
        linea, _, resto = bytes(self.buffer).partition(b"\n")
        self.buffer = bytearray(resto)
        return linea

    def stderr(self):
        """Lo último del stderr. Antes lee, sin esperar, lo que quedó en el pipe (se llama con el
        grupo del hijo ya muerto; el tope de lecturas es por si algo escapó del grupo y sigue escribiendo)."""
        for _ in range(256):
            try:
                if not self._leer_stderr():
                    break
            except (BlockingIOError, OSError):
                break
        return bytes(self.cola_stderr).decode("utf-8", errors="replace")

    def cerrar(self):
        self.selector.close()


def _ultima_linea(texto):
    lineas = [l for l in texto.splitlines() if l.strip()]
    return lineas[-1].strip() if lineas else ""


def _causa_de_la_excepcion(error):
    """La causa según la excepción con la que terminó el traceback del hijo."""
    tipo = _ultima_linea(error).split(":", 1)[0].rsplit(".", 1)[-1]
    return {"MemoryError": "memoria", "ManifiestoInvalido": "manifiesto"}.get(tipo, "excepcion")


class ConectorEnSubproceso:
    """Un conector que corre en un proceso hijo, una muestra por proceso.

    Se usa como cualquier conector: procesar(ConectorEnSubproceso(carpeta, sdk_dir), ctx, msg).
    popen se pasa tal cual a subprocess.Popen (en AURA: user, env y cwd). Después de cada muestra,
    stderr tiene lo último que escribió el hijo (prints y log del conector), hasta 4 KB."""

    def __init__(self, carpeta: str, sdk_dir: str, *, timeout_s: float = 10, limites: Limites = Limites(),
                 python: str = sys.executable, popen: Optional[dict] = None):
        self.carpeta = str(Path(carpeta).resolve())
        self.sdk_dir = str(Path(sdk_dir).resolve())
        self.timeout_s = timeout_s
        self.limites = limites
        self.python = python
        self.popen = dict(popen or {})
        self.stderr = ""

    def comando(self) -> list:
        comando = [self.python, "-I", str(Path(self.sdk_dir) / "aura_sdk" / "subproceso.py"),
                   "--mem-mb", str(self.limites.mem_mb), "--cpu-s", str(self.limites.cpu_s),
                   "--archivos", str(self.limites.archivos)]
        if self.limites.procesos is not None:
            comando += ["--procesos", str(self.limites.procesos)]
        return comando

    def al_recibir_datos(self, msg: Mensaje, ctx: Contexto) -> None:
        inicio = time.monotonic()
        self.stderr = ""
        # Grupo de procesos propio (sin preexec_fn): al terminar se mata el grupo entero, también
        # lo que el conector haya lanzado.
        opciones = {**self.popen, "process_group": 0}
        try:
            proc = subprocess.Popen(self.comando(), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, **opciones)
        except PermissionError as e:
            raise FalloDelConector("sin_permisos", f"no se pudo lanzar el subproceso: {e}") from None
        except OSError as e:
            raise FalloDelConector("murio", f"no se pudo lanzar el subproceso: {e}") from None
        sesion = _Sesion(proc, inicio + self.timeout_s)
        try:
            self._conversar(sesion, msg, ctx, inicio)
        except _Vencido:
            raise FalloDelConector("timeout", f"no terminó en {self.timeout_s} s") from None
        finally:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            proc.wait()
            self.stderr = sesion.stderr()
            sesion.cerrar()
            for f in (proc.stdin, proc.stdout, proc.stderr):
                f.close()

    def _conversar(self, sesion, msg, ctx, inicio):
        sesion.enviar(codificar({"tipo": "inicio", "carpeta": self.carpeta, "mensaje": {
            "hw_id": msg.hw_id, "values": msg.values, "ts": msg.ts,
            "ingest_id": msg.ingest_id, "adaptador": msg.adaptador}}))
        while True:
            linea = sesion.linea()
            if linea is None:
                raise self._murio(sesion, inicio)
            try:
                m = decodificar(linea.decode("utf-8"))
            except (ValueError, RecursionError):   # RecursionError: JSON anidado a propósito
                raise FalloDelConector("protocolo", f"una línea que no es JSON: {linea[:200]!r}") from None
            tipo = m.get("tipo")
            if tipo == "fin":
                if m.get("ok") is True:
                    return
                error = m.get("error") if isinstance(m.get("error"), str) else repr(m.get("error"))
                raise FalloDelConector(_causa_de_la_excepcion(error), error)
            if tipo != "llamada" or m.get("metodo") not in METODOS or not isinstance(m.get("args"), dict):
                raise FalloDelConector("protocolo", f"mensaje inválido: {linea[:200]!r}")
            sesion.enviar(self._atender(ctx, m["metodo"], m["args"]))

    @staticmethod
    def _atender(ctx, metodo, args) -> str:
        """Llama al Contexto y devuelve la respuesta ya codificada."""
        try:
            valor = getattr(ctx, metodo)(**args)
        except tuple(ERRORES.values()) as e:
            nombre = next(n for n, t in ERRORES.items() if isinstance(e, t))   # una subclase cruza como su base
            detalle = e.args[0] if isinstance(e, KeyError) and e.args else e   # str(KeyError) agrega comillas
            return codificar({"tipo": "respuesta", "ok": False, "error": nombre, "detalle": str(detalle)})
        except Exception as e:
            # Falla de AURA (por ejemplo, la base): no se le pasa al conector. Lo mata el finally.
            raise ErrorDelAnfitrion(e) from e
        try:
            return codificar({"tipo": "respuesta", "ok": True, "valor": valor})
        except (TypeError, ValueError, RecursionError) as e:
            # El Contexto devolvió algo que no es JSON (un Decimal, por ejemplo): bug de AURA.
            raise ErrorDelAnfitrion(e) from e

    def _murio(self, sesion, inicio):
        """Cerró su stdout sin mandar fin: por qué."""
        sesion.esperar_fin()   # si cerró stdout pero sigue vivo, vence y cuenta como timeout
        codigo = sesion.proc.returncode
        if codigo < 0:
            senal = signal.Signals(-codigo).name if -codigo in signal.valid_signals() else str(-codigo)
            if -codigo in (signal.SIGKILL, signal.SIGXCPU) and time.monotonic() - inicio >= self.limites.cpu_s:
                return FalloDelConector("timeout", f"superó el límite de CPU ({self.limites.cpu_s} s): señal {senal}")
            return FalloDelConector("murio", f"terminó con la señal {senal} sin mandar fin")
        return FalloDelConector("murio", f"terminó con código {codigo} sin mandar fin")


if __name__ == "__main__":   # python -m aura_sdk.subproceso
    sys.exit(main())
