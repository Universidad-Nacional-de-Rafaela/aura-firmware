"""Tests del conector de E2-AZ-TERR-TAN01. Correr desde la raíz del repo:

    python -m unittest discover -s conectores/E2-AZ-TERR-TAN01/tests
"""
import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

CARPETA = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CARPETA.parent.parent / "comun" / "python"))   # aura_sdk
from aura_sdk import ContextoDePrueba, Mensaje, cargar_conector, procesar  # noqa: E402

T0 = datetime(2026, 10, 9, 12, 0, tzinfo=timezone.utc)
SIN_ECO = 65535   # 0xFFFF: lo que manda el firmware si el sensor no responde


def msg(values, minuto=0):
    return Mensaje(hw_id="eui-19e2db6c14a8178c", values=values, ts=T0 + timedelta(minutes=minuto),
                   ingest_id=f"id-{minuto}")


class ConectorTanques(unittest.TestCase):
    def setUp(self):
        self.manifiesto, self.conector = cargar_conector(CARPETA)
        self.ctx = ContextoDePrueba(self.manifiesto)
        self.umbral = self.conector.UMBRAL_BAJO_MM

    def test_guarda_los_tres_tanques(self):
        valores = {"distancia_tanque1_mm": 1500, "distancia_tanque2_mm": 2000, "distancia_tanque3_mm": 3000}
        r = procesar(self.conector, self.ctx, msg(valores))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("distancia_tanque2_mm"), [(T0, 2000)])

    def test_un_sensor_con_falla_no_se_guarda_pero_los_demas_si(self):
        valores = {"distancia_tanque1_mm": 1500, "distancia_tanque2_mm": SIN_ECO, "distancia_tanque3_mm": 3000}
        r = procesar(self.conector, self.ctx, msg(valores))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(r.quitados, ["distancia_tanque2_mm"])
        self.assertEqual(self.ctx.serie("distancia_tanque2_mm"), [])
        self.assertEqual(len(self.ctx.serie("distancia_tanque1_mm")), 1)

    def test_todos_con_falla_se_descarta(self):
        valores = {f"distancia_tanque{i}_mm": SIN_ECO for i in (1, 2, 3)}
        r = procesar(self.conector, self.ctx, msg(valores))
        self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_campos_validos"))

    def test_fuera_de_rango_no_se_guarda(self):
        for minuto, valor in enumerate((249, 6001)):
            r = procesar(self.conector, self.ctx, msg({"distancia_tanque1_mm": valor}, minuto))
            self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_campos_validos"))

    def test_los_limites_del_rango_si_se_guardan(self):
        for minuto, valor in enumerate((250, 6000)):
            r = procesar(self.conector, self.ctx, msg({"distancia_tanque1_mm": valor}, minuto))
            self.assertEqual(r.resultado, "persistido")

    def test_un_campo_que_no_esta_en_el_manifiesto_no_se_guarda(self):
        r = procesar(self.conector, self.ctx, msg({"contador": 7}))
        self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_campos_validos"))

    def test_alerta_una_vez_por_cruce(self):
        alto = self.umbral + 500
        bajo = self.umbral - 500
        for minuto, valor in enumerate([bajo, alto, alto + 100, alto, bajo, bajo]):
            procesar(self.conector, self.ctx, msg({"distancia_tanque1_mm": valor}, minuto))
        self.assertEqual([(a.tipo, a.severidad) for a in self.ctx.alertas],
                         [("tanque_bajo", "warning"), ("tanque_bajo", "info")])

    def test_cada_tanque_alerta_por_separado(self):
        alto = self.umbral + 500
        procesar(self.conector, self.ctx, msg({"distancia_tanque1_mm": alto, "distancia_tanque3_mm": 1000}))
        self.assertEqual(len(self.ctx.alertas), 1)
        self.assertIn("Tanque 1", self.ctx.alertas[0].mensaje)

    def test_un_tanque_bajo_desde_la_primera_medicion_alerta(self):
        procesar(self.conector, self.ctx, msg({"distancia_tanque2_mm": self.umbral + 1}))
        self.assertEqual([a.severidad for a in self.ctx.alertas], ["warning"])
        self.assertIn("Tanque 2", self.ctx.alertas[0].mensaje)


if __name__ == "__main__":
    unittest.main()
