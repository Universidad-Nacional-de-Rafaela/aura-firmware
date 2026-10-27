"""Conector de prueba: intenta crear un proceso y guarda si pudo (2) o no (1)."""
import os

from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        try:
            pid = os.fork()
        except OSError:
            ctx.guardar_medicion({"temp_c": 1})
            return
        if pid == 0:
            os._exit(0)
        os.waitpid(pid, 0)
        ctx.guardar_medicion({"temp_c": 2})
