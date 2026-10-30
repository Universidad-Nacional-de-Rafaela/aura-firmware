"""Conector de prueba: llama a serie() y después duerme."""
import time

from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        ctx.serie("temp_c")
        time.sleep(5)
