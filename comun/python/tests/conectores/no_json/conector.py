"""Conector de prueba: escribe una línea que no es JSON en el stdout real."""
import sys

from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        sys.__stdout__.write("esto no es JSON\n")
        sys.__stdout__.flush()
        ctx.guardar_medicion(msg.values)
