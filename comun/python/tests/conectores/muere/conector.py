"""Conector de prueba: termina el proceso sin mandar fin."""
import os

from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        os._exit(3)
