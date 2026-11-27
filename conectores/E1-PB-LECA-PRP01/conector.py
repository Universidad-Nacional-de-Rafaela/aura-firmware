"""Conector de E1-PB-LECA-PRP01 — presencia de personas en el aula, detectada por cámara.

La hoja manda presencia como entero: 1 (hay personas) o 0 (no hay). `conf_pct` acompaña
si viene. Los booleanos, flotantes y demás valores de presencia se descartan.

Ejemplos:
    {"presencia": 1, "conf_pct": 72}       -> guarda {"presencia": 1, "conf_pct": 72}
    {"presencia": 0}                       -> guarda {"presencia": 0}
    {"presencia": true}                    -> descarta (presencia_invalida)
    {"presencia": "si"}                   -> descarta (presencia_invalida)
    {"conf_pct": 80}                      -> descarta (sin_presencia)

Cuando la cámara no da una lectura confiable, la hoja no manda dato (manda la alerta `sensor`),
así que acá nunca se inventa un 0: si no llega `presencia`, no se guarda nada.
"""
from aura_sdk import Conector


class ConectorPresencia(Conector):
    def al_recibir_datos(self, msg, ctx):
        if "presencia" not in msg.values:
            ctx.descartar("sin_presencia")
            return
        presencia = msg.values["presencia"]
        # bool hereda de int en Python: exigir el tipo exacto evita aceptar true/false.
        if type(presencia) is not int or presencia not in (0, 1):
            ctx.descartar("presencia_invalida")
            return

        values = {"presencia": presencia}
        if "conf_pct" in msg.values:
            values["conf_pct"] = msg.values["conf_pct"]   # AURA quita tipos y rangos inválidos
        ctx.guardar_medicion(values, msg.ts)
