"""Conector de E1-PB-LECA-PRE01 — presencia de personas en el aula, detectada por cámara.

La hoja manda {"presencia": true|false, "conf_pct": 72}. AURA guarda solo números, así que
`presencia` se guarda como 1 (hay personas) o 0 (no hay). `conf_pct` acompaña si viene.

Ejemplos:
    {"presencia": true,  "conf_pct": 72}  -> guarda {"presencia": 1, "conf_pct": 72}
    {"presencia": false}                  -> guarda {"presencia": 0}
    {"presencia": "si"}                   -> descarta (presencia_invalida)
    {"conf_pct": 80}                      -> descarta (sin_presencia)

Cuando la cámara no da una lectura confiable, la hoja no manda dato (manda la alerta `sensor`),
así que acá nunca se inventa un 0: si no llega `presencia`, no se guarda nada.
"""
from aura_sdk import Conector


def presencia_a_numero(valor):
    """True/False (o 1/0) -> 1/0. Cualquier otra cosa -> None."""
    if isinstance(valor, bool):
        return 1 if valor else 0
    if isinstance(valor, int) and valor in (0, 1):
        return valor
    return None


class ConectorPresencia(Conector):
    def al_recibir_datos(self, msg, ctx):
        if "presencia" not in msg.values:
            ctx.descartar("sin_presencia")
            return
        presencia = presencia_a_numero(msg.values["presencia"])
        if presencia is None:
            ctx.descartar("presencia_invalida")
            return

        values = {"presencia": presencia}
        if "conf_pct" in msg.values:
            values["conf_pct"] = msg.values["conf_pct"]   # si está fuera de rango, AURA lo quita
        ctx.guardar_medicion(values, msg.ts)
