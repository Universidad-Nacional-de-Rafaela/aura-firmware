"""procesar(): lo que hace AURA con cada mensaje de un dispositivo, en chico.

El runtime de AURA hace lo mismo con su Contexto real y una transacción por mensaje. Con
ContextoDePrueba sirve para los tests de un conector y para herramientas/correr_conector.py.
"""
import traceback
from dataclasses import dataclass, field
from typing import Optional

MOTIVO_FALLO = "conector_fallo"


class FalloDelConector(Exception):
    """El conector no pudo procesar la muestra (lo levanta el runner que lo corre aparte).

    causa: "excepcion", "timeout", "memoria", "murio", "protocolo", "manifiesto",
    "sin_permisos" o "conector_no_disponible". detalle: texto para el log y la alerta."""

    def __init__(self, causa: str, detalle: str = ""):
        super().__init__(f"{causa}: {detalle}")
        self.causa = causa
        self.detalle = detalle


class ErrorDelAnfitrion(Exception):
    """Falló AURA, no el conector: un método del Contexto lanzó algo que no es un error de uso
    (por ejemplo, la base no contesta durante serie()). procesar() deshace y relanza `original`:
    es un error transitorio de la ingesta (sin ack, el nodo reintenta), no una falla del conector."""

    def __init__(self, original: BaseException):
        super().__init__(repr(original))
        self.original = original


@dataclass
class ResultadoMensaje:
    resultado: Optional[str]           # el ack que publicaría AURA: persistido, descartado o duplicado
    motivo: Optional[str] = None       # "conector_fallo" si el conector falló (D5)
    quitados: list = field(default_factory=list)
    error: Optional[str] = None        # traceback o detalle, si el conector falló


def procesar(conector, ctx, msg) -> ResultadoMensaje:
    """Pasa un Mensaje por el conector y devuelve qué contestaría AURA.

    - ingest_id ya guardado          -> "duplicado", sin llamar al conector
    - el conector guardó             -> "persistido"
    - descartó, o no hizo nada       -> "descartado" (motivo, o "sin_accion")
    - falló AURA (ErrorDelAnfitrion) -> deshace y relanza el error original: no es culpa del conector
    - falló (excepción, timeout...)  -> D5: no queda nada de lo que hizo el conector; se guarda la
                                        muestra cruda (solo los campos dentro de rango) con una alerta
                                        "conector_fallo": "persistido", o "descartado" si no hay ningún
                                        campo válido; motivo "conector_fallo". El nodo no reenvía.
    """
    if ctx._ya_visto(msg.ingest_id):
        return ResultadoMensaje("duplicado")
    ctx._empezar(msg)
    try:
        conector.al_recibir_datos(msg, ctx)
    except ErrorDelAnfitrion as e:
        ctx._deshacer()
        raise e.original
    except Exception as e:
        ctx._deshacer()
        if isinstance(e, FalloDelConector):
            causa, error = e.causa, e.detalle
        else:
            causa, error = "excepcion", traceback.format_exc()
        ctx.log.error("el conector falló con %s (%s): se guarda la muestra cruda\n%s", msg.ingest_id, causa, error)
        ctx._empezar(msg)
        ctx.guardar_medicion(msg.values, msg.ts)
        ctx._fallo(causa, error)
        resultado, _, quitados = ctx._confirmar()
        return ResultadoMensaje(resultado, MOTIVO_FALLO, list(quitados), error)
    resultado, motivo, quitados = ctx._confirmar()
    return ResultadoMensaje(resultado, motivo, list(quitados))
