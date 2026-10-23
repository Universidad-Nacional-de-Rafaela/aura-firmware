"""Conector de prueba: lee la configuración vigente y guarda."""
from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        ctx.config_vigente()
        ctx.guardar_medicion(msg.values)
