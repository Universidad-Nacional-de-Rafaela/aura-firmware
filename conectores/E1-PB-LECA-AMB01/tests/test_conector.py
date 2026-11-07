"""Tests del conector, sin AURA ni placa. Correr desde la raíz del repo:

    python3 -m unittest discover -s conectores/E1-PB-LECA-AMB01/tests
"""
import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

CARPETA = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CARPETA.parent.parent / "comun" / "python"))   # aura_sdk
from aura_sdk import ContextoDePrueba, Mensaje, cargar_conector, procesar  # noqa: E402

T0 = datetime(2026, 11, 4, 12, 0, tzinfo=timezone.utc)
HW_ID = "mac-907069abbfe4"


def msg(values, minuto=0):
    return Mensaje(hw_id=HW_ID, values=values, ts=T0 + timedelta(minutes=minuto), ingest_id=f"id-{minuto}")


class ConectorCO2(unittest.TestCase):
    def setUp(self):
        self.manifiesto, self.conector = cargar_conector(CARPETA)
        self.ctx = ContextoDePrueba(self.manifiesto)

    def correr(self, valores, ctx=None):
        for i, ppm in enumerate(valores):
            procesar(self.conector, ctx or self.ctx, msg({"co2_ppm": ppm}, i))

    def avisos(self, ctx=None):
        return [(a.tipo, a.severidad) for a in (ctx or self.ctx).alertas]

    def test_guarda_una_medicion_valida(self):
        r = procesar(self.conector, self.ctx, msg({"co2_ppm": 812}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("co2_ppm"), [(T0, 812)])

    def test_fuera_de_rango_no_se_guarda_ni_alerta(self):
        for i, ppm in enumerate((399, 5001, 0)):
            r = procesar(self.conector, self.ctx, msg({"co2_ppm": ppm}, i))
            self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_campos_validos"))
        self.assertEqual(self.ctx.serie("co2_ppm"), [])
        self.assertEqual(self.avisos(), [])

    def test_en_verde_no_avisa(self):
        self.correr([450, 800, 1000, 900])
        self.assertEqual(self.avisos(), [])

    def test_1000_justo_es_verde_y_1001_amarillo(self):
        self.correr([1000, 1001])
        self.assertEqual(self.avisos(), [("aire_atencion", "warning")])

    def test_amarillo_avisa_una_vez_y_vuelve(self):
        self.correr([900, 1100, 1200, 1400, 1500, 950])
        self.assertEqual(self.avisos(), [("aire_atencion", "warning"), ("aire_atencion", "info")])

    def test_rojo_avisa_una_vez_y_vuelve(self):
        self.correr([800, 1600, 1700, 5000, 700])
        self.assertEqual(self.avisos(), [("aire_alto", "high"), ("aire_alto", "info")])
        self.assertIn("1600 ppm", self.ctx.alertas[0].mensaje)

    def test_1500_justo_es_amarillo_y_1501_rojo(self):
        self.correr([1200, 1500, 1501])
        self.assertEqual(self.avisos(), [("aire_atencion", "warning"), ("aire_alto", "high")])

    def test_rojo_a_amarillo_avisa_que_sigue_en_atencion(self):
        self.correr([800, 1700, 1300])
        self.assertEqual(self.avisos(), [("aire_alto", "high"), ("aire_alto", "info")])
        self.assertIn("sigue en atención", self.ctx.alertas[1].mensaje)

    def test_primera_medicion_en_rojo_alerta(self):
        self.correr([2000])
        self.assertEqual(self.avisos(), [("aire_alto", "high")])

    def test_usa_los_umbrales_que_reporto_la_placa(self):
        ctx = ContextoDePrueba(self.manifiesto, config={"atencion_ppm": 700, "alto_ppm": 900})
        self.correr([600, 750, 950], ctx)
        self.assertEqual(self.avisos(ctx), [("aire_atencion", "warning"), ("aire_alto", "high")])

    def test_umbrales_invalidos_se_ignoran(self):
        for config in ({"atencion_ppm": 1600, "alto_ppm": 1000}, {"atencion_ppm": "mil"}, {"alto_ppm": 99999}):
            ctx = ContextoDePrueba(self.manifiesto, config=config)
            self.correr([800, 1200], ctx)
            self.assertEqual(self.avisos(ctx), [("aire_atencion", "warning")])


if __name__ == "__main__":
    unittest.main()