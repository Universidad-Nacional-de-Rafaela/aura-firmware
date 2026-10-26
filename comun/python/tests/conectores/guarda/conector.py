"""Conector de prueba: lee la serie, revisa los tipos y guarda."""
from datetime import datetime

from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        s = ctx.serie("temp_c")
        for punto in s:
            if not (isinstance(punto, tuple) and isinstance(punto[0], datetime) and isinstance(punto[1], float)):
                raise TypeError(f"la serie no trae (datetime, float): {punto!r}")
        ctx.guardar_medicion({"temp_c": msg.values["temp_c"], "hum_pct": len(s)}, msg.ts)
