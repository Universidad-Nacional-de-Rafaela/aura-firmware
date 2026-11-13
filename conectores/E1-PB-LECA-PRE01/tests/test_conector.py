"""Tests del conector. Correr desde la raíz del repo:

    python3 -m unittest discover -s conectores/E1-PB-LECA-PRE01/tests
"""
import sys
import unittest
from datetime import datetime, timezone
from pathlib import Path

CARPETA = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CARPETA.parent.parent / "comun" / "python"))   # aura_sdk
from aura_sdk import ContextoDePrueba, Mensaje, cargar_conector, procesar  # noqa: E402


def msg(values, ingest_id="id-1"):
    return Mensaje(hw_id="mac-dca632aabbcc", values=values, ingest_id=ingest_id,
                   ts=datetime(2026, 11, 16, 12, 0, tzinfo=timezone.utc))


class Conector(unittest.TestCase):
    def setUp(self):
        self.manifiesto, self.conector = cargar_conector(CARPETA)
        self.ctx = ContextoDePrueba(self.manifiesto)

    def ultima(self):
        return self.ctx.mediciones[-1].values

    def test_hay_personas_se_guarda_como_1(self):
        r = procesar(self.conector, self.ctx, msg({"presencia": True, "conf_pct": 72}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ultima(), {"presencia": 1, "conf_pct": 72})

    def test_sala_vacia_se_guarda_como_0(self):
        r = procesar(self.conector, self.ctx, msg({"presencia": False}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ultima(), {"presencia": 0})

    def test_confianza_fuera_de_rango_se_quita_pero_la_presencia_queda(self):
        r = procesar(self.conector, self.ctx, msg({"presencia": True, "conf_pct": 150}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ultima(), {"presencia": 1})

    def test_presencia_invalida_se_descarta(self):
        for valor in ("si", 2, 0.5, None):
            with self.subTest(valor=valor):
                r = procesar(self.conector, self.ctx, msg({"presencia": valor}, ingest_id=f"id-{valor}"))
                self.assertEqual(r.resultado, "descartado")

    def test_sin_presencia_se_descarta_y_no_se_inventa_un_cero(self):
        r = procesar(self.conector, self.ctx, msg({"conf_pct": 80}))
        self.assertEqual(r.resultado, "descartado")
        self.assertEqual(self.ctx.mediciones, [])


if __name__ == "__main__":
    unittest.main()
