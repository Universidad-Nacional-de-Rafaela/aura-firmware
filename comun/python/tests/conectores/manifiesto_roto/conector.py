"""Conector de prueba: su manifiesto no tiene campos."""
from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        ctx.guardar_medicion(msg.values)
