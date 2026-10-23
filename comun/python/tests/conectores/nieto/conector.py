"""Conector de prueba: lanza un proceso que queda durmiendo y después se cuelga."""
import subprocess
import sys
import time

from aura_sdk import Conector


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        nieto = subprocess.Popen(["sleep", "30"])
        print(f"nieto={nieto.pid}", file=sys.stderr, flush=True)
        time.sleep(30)
