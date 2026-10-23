"""Conector de prueba: no termina nunca."""
from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        while True:
            pass
