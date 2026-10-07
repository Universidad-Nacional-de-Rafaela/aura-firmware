"""procesar(): lo que hace AURA con cada mensaje de un dispositivo, en chico.

El runtime de AURA hace lo mismo con su Contexto real y una transacción por mensaje. Con
ContextoDePrueba sirve para los tests de un conector y para herramientas/correr_conector.py.
"""
import traceback
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class ResultadoMensaje:
    resultado: Optional[str]           # el ack que publicaría AURA; None = sin ack (se reintenta)
    motivo: Optional[str] = None
    quitados: list = field(default_factory=list)
    error: Optional[str] = None        # traceback, si el conector lanzó una excepción


def procesar(conector, ctx, msg) -> ResultadoMensaje:
    """Pasa un Mensaje por el conector y devuelve qué contestaría AURA.

    - ingest_id ya guardado          -> "duplicado", sin llamar al conector
    - el conector guardó             -> "persistido"
    - descartó, o no hizo nada       -> "descartado" (motivo, o "sin_accion")
    - lanzó una excepción            -> sin ack: no queda nada guardado y el nodo reintenta
    """
    if ctx._ya_visto(msg.ingest_id):
        return ResultadoMensaje("duplicado")
    ctx._empezar(msg)
    try:
        conector.al_recibir_datos(msg, ctx)
    except Exception:
        ctx._deshacer()
        error = traceback.format_exc()
        ctx.log.error("el conector falló con %s: sin ack, el dispositivo va a reenviar\n%s", msg.ingest_id, error)
        return ResultadoMensaje(None, error=error)
    resultado, motivo, quitados = ctx._confirmar()
    return ResultadoMensaje(resultado, motivo, list(quitados))
