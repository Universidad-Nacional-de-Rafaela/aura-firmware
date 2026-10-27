"""Conector de prueba: un error de ctx llega con su tipo."""
from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        try:
            ctx.serie("no_declarado")
        except KeyError:
            ctx.guardar_medicion(msg.values)
            return
        raise AssertionError("serie() de un campo no declarado tenía que lanzar KeyError")
