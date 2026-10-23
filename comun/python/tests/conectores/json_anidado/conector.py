"""Conector de prueba: escribe en el stdout real un JSON anidado 200 000 niveles."""
import sys

from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        sys.__stdout__.write("[" * 200000 + "\n")
        sys.__stdout__.flush()
        ctx.guardar_medicion(msg.values)
