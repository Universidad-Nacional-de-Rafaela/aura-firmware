"""Conector de prueba: reserva 1 GB."""
from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        bytearray(1 << 30)
