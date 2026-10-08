"""Tests del conector. Correr desde la raíz del repo:

    python3 -m unittest discover -s conectores/<CÓDIGO>/tests
"""
import sys
import unittest
from datetime import datetime, timezone
from pathlib import Path

CARPETA = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CARPETA.parent.parent / "comun" / "python"))   # aura_sdk
from aura_sdk import ContextoDePrueba, Mensaje, cargar_conector, procesar  # noqa: E402


def msg(values, ingest_id="id-1"):
    return Mensaje(hw_id="mac-e072a1f7efe4", values=values, ingest_id=ingest_id,
                   ts=datetime(2026, 10, 7, 12, 0, tzinfo=timezone.utc))


class Conector(unittest.TestCase):
    def setUp(self):
        self.manifiesto, self.conector = cargar_conector(CARPETA)
        self.ctx = ContextoDePrueba(self.manifiesto)

    def test_guarda_una_medicion_valida(self):
        r = procesar(self.conector, self.ctx, msg({"temp_c": 4.5}))   # TODO: tus campos
        self.assertEqual(r.resultado, "persistido")

    def test_una_medicion_fuera_de_rango_no_se_guarda(self):
        r = procesar(self.conector, self.ctx, msg({"temp_c": 999}))   # TODO: tus campos
        self.assertEqual(r.resultado, "descartado")

    # TODO: un test por cada regla propia de tu conector (alertas, descartes, predicción).


if __name__ == "__main__":
    unittest.main()
