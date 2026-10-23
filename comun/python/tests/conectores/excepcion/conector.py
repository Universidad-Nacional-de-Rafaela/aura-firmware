"""Conector de prueba: lanza una excepción."""
from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        raise ValueError("bug del grupo")
