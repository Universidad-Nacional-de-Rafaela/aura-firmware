"""Tests del conector E1-PB-LECA-CRT01, sin AURA ni placa. Correr desde la raíz del repo:

    python3 -m unittest discover -s conectores/E1-PB-LECA-CRT01/tests
"""
import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

CARPETA = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CARPETA.parent.parent / "comun" / "python"))   # aura_sdk
from aura_sdk import ContextoDePrueba, Mensaje, cargar_conector, procesar  # noqa: E402

T0 = datetime(2026, 11, 16, 12, 0, tzinfo=timezone.utc)
HW_ID = "mac-907069ab967c"
TIPO = "motor_atascado"


def msg(values, seg=0):
    return Mensaje(hw_id=HW_ID, values=values, ts=T0 + timedelta(seconds=seg), ingest_id=f"id-{seg}")


class ConectorCortinaTest(unittest.TestCase):
    def setUp(self):
        self.manifiesto, self.conector = cargar_conector(CARPETA)
        self.ctx = ContextoDePrueba(self.manifiesto)

    def avisos(self):
        return [(a.tipo, a.severidad) for a in self.ctx.alertas]

    def test_guarda_medicion_completa_valida(self):
        r = procesar(self.conector, self.ctx, msg({
            "posicion_pct": 50,
            "luz_lux": 420.5,
            "estado_motor": 0,
            "modo": 1,
        }))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("posicion_pct"), [(T0, 50)])
        self.assertEqual(self.ctx.serie("luz_lux"), [(T0, 420.5)])

    def test_guarda_medicion_sin_luz_cuando_sonda_falla(self):
        r = procesar(self.conector, self.ctx, msg({"posicion_pct": 0, "estado_motor": 0, "modo": 0}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("luz_lux"), [])

    def test_fuera_de_rango_se_descarta(self):
        r = procesar(self.conector, self.ctx, msg({
            "posicion_pct": 150,
            "luz_lux": -5,
            "estado_motor": 9,
            "modo": -1,
        }))
        self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_campos_validos"))

    def test_movimiento_corto_no_alerta(self):
        for seg, est in ((0, 0), (10, 1), (20, 1), (30, 0)):
            procesar(self.conector, self.ctx, msg({"posicion_pct": 50, "estado_motor": est}, seg))
        self.assertEqual(self.avisos(), [])

    def test_motor_atascado_alerta_una_vez_y_cierra_al_detenerse(self):
        for seg, est in ((0, 0), (10, 1), (100, 1), (190, 1), (250, 1), (260, 0)):
            procesar(self.conector, self.ctx, msg({"posicion_pct": 50, "estado_motor": est}, seg))
        self.assertEqual(self.avisos(), [(TIPO, "high"), (TIPO, "info")])


if __name__ == "__main__":
    unittest.main()
