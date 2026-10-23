"""Conector de prueba: escribe unos 50 MB en stderr y guarda."""
import sys

from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        bloque = "x" * (1 << 20)
        for _ in range(50):
            sys.stderr.write(bloque)
        sys.stderr.write("\nFIN-DEL-STDERR\n")
        sys.stderr.flush()
        ctx.guardar_medicion(msg.values)
