"""Tests del conector, sin AURA ni placa. Correr desde la raíz del repo:

    python3 -m unittest discover -s ejemplos/conector_ejemplo/tests
"""
import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

CARPETA = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CARPETA.parent.parent / "comun" / "python"))   # aura_sdk
from aura_sdk import ContextoDePrueba, Mensaje, cargar_conector, procesar  # noqa: E402

T0 = datetime(2026, 10, 7, 12, 0, tzinfo=timezone.utc)


def msg(temp, minuto, n=None):
    return Mensaje(hw_id="mac-e072a1f7efe4", values={"temp_chip_c": temp}, ts=T0 + timedelta(minutes=minuto),
                   ingest_id=f"id-{n if n is not None else minuto}")


class ConectorEjemplo(unittest.TestCase):
    def setUp(self):
        self.manifiesto, self.conector = cargar_conector(CARPETA)
        self.ctx = ContextoDePrueba(self.manifiesto)

    def test_guarda(self):
        r = procesar(self.conector, self.ctx, msg(35.2, 0))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("temp_chip_c"), [(T0, 35.2)])

    def test_fuera_de_rango_se_descarta(self):
        r = procesar(self.conector, self.ctx, msg(300, 0))
        self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_campos_validos"))

    def test_alerta_una_vez_por_cruce(self):
        for i, t in enumerate([50, 61, 65, 70, 55, 52]):
            procesar(self.conector, self.ctx, msg(t, i))
        self.assertEqual([(a.tipo, a.severidad) for a in self.ctx.alertas],
                         [("temperatura_alta", "warning"), ("temperatura_alta", "info")])

    def test_prediccion_de_una_recta(self):
        ahora = datetime.now(timezone.utc)
        for i in range(10):   # sube 1 °C cada 5 minutos en la última hora
            ts = ahora - timedelta(minutes=50 - 5 * i)
            procesar(self.conector, self.ctx, Mensaje(hw_id="mac-e072a1f7efe4", values={"temp_chip_c": 30.0 + i},
                                                      ts=ts, ingest_id=f"p{i}"))
        self.conector.predecir(self.ctx)
        p = self.ctx.predicciones[0]
        self.assertEqual(p.modelo, "recta_1h")
        # el último punto (39 °C) es de hace 5 min: ahora la recta da 40 °C, y en una hora, 52 °C
        self.assertAlmostEqual(p.valor, 52.0, delta=0.5)
        self.assertLessEqual(p.inferior, p.valor)
        self.assertGreaterEqual(p.superior, p.valor)

    def test_sin_datos_no_predice(self):
        self.conector.predecir(self.ctx)
        self.assertEqual(self.ctx.predicciones, [])


if __name__ == "__main__":
    unittest.main()
