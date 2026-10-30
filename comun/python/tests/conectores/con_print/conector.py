"""Conector de prueba: print() antes y después de guardar."""
from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        print("hola")
        ctx.guardar_medicion(msg.values)
        print("hola")
