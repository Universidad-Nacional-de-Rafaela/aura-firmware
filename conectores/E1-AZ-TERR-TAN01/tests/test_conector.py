"""Tests del conector, sin AURA ni placa. Correr desde la raíz del repo:

    python3 -m unittest discover -s conectores/E1-AZ-TERR-TAN01/tests
"""
import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

CARPETA = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CARPETA.parent.parent / "comun" / "python"))   # aura_sdk
from aura_sdk import ContextoDePrueba, Mensaje, cargar_conector, procesar  # noqa: E402

T0 = datetime(2026, 10, 7, 12, 0, tzinfo=timezone.utc)
HW_ID = "eui-798a381dd8c1cba4"


def msg(values, minuto=0):
    return Mensaje(hw_id=HW_ID, values=values, ts=T0 + timedelta(minutes=minuto),
                   ingest_id=f"id-{minuto}", adaptador="svc-lorawan-bridge")


def dist(n, pct):
    """La distancia que corresponde a un nivel, con las medidas de TANQUES de conector.py."""
    vacio, lleno = {1: (137, 40), 2: (152.5, 25)}[n]
    return vacio - pct / 100 * (vacio - lleno)


def tanques(n1, n2, minuto=0):
    """Un uplink completo, como lo devuelve codec.js, para esos niveles en %."""
    return msg({"dist_t1_cm": dist(1, n1), "dist_t2_cm": dist(2, n2)}, minuto)


class ConectorTanques(unittest.TestCase):
    def setUp(self):
        self.manifiesto, self.conector = cargar_conector(CARPETA)
        self.ctx = ContextoDePrueba(self.manifiesto)

    def ultimo(self, campo):
        return self.ctx.serie(campo)[-1][1]

    def test_calcula_nivel_y_volumen_de_los_dos_tanques(self):
        r = procesar(self.conector, self.ctx, msg({"dist_t1_cm": 59.4, "dist_t2_cm": 88.75}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("dist_t1_cm"), [(T0, 59.4)])
        self.assertEqual(self.ctx.serie("nivel_t1_pct"), [(T0, 80.0)])   # (137-59.4)/(137-40)
        self.assertEqual(self.ctx.serie("vol_t1_l"), [(T0, 800)])
        self.assertEqual(self.ctx.serie("nivel_t2_pct"), [(T0, 50.0)])   # (152.5-88.75)/(152.5-25)
        self.assertEqual(self.ctx.serie("vol_t2_l"), [(T0, 250)])

    def test_fuera_de_vacio_lleno_se_acota(self):
        procesar(self.conector, self.ctx, msg({"dist_t1_cm": 15.0, "dist_t2_cm": 210.0}))
        self.assertEqual((self.ultimo("nivel_t1_pct"), self.ultimo("vol_t1_l")), (100.0, 1000))
        self.assertEqual((self.ultimo("nivel_t2_pct"), self.ultimo("vol_t2_l")), (0.0, 0))

    def test_un_sensor_que_falla_no_aparece_y_el_resto_se_guarda(self):
        # codec.js omite la distancia sin lectura: llega solo el tanque 2
        r = procesar(self.conector, self.ctx, msg({"dist_t2_cm": 88.75}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("nivel_t1_pct"), [])
        self.assertEqual(self.ctx.serie("vol_t1_l"), [])
        self.assertEqual(self.ctx.serie("nivel_t2_pct"), [(T0, 50.0)])

    def test_distancia_fuera_de_rango_no_calcula_nada(self):
        r = procesar(self.conector, self.ctx, msg({"dist_t1_cm": 700.0, "dist_t2_cm": 88.75}))
        self.assertEqual(r.resultado, "persistido")
        self.assertIn("dist_t1_cm", r.quitados)
        self.assertEqual(self.ctx.serie("nivel_t1_pct"), [])
        self.assertEqual(self.ctx.serie("nivel_t2_pct"), [(T0, 50.0)])

    def test_nada_valido_se_descarta(self):
        r = procesar(self.conector, self.ctx, msg({"dist_t1_cm": 700.0}))
        self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_campos_validos"))

    def test_nivel_y_volumen_del_mensaje_se_ignoran(self):
        # los calcula el conector: si llegan en values (un codec viejo), no se usan
        procesar(self.conector, self.ctx, msg({"dist_t1_cm": 59.4, "nivel_t1_pct": 5, "vol_t1_l": 50}))
        self.assertEqual(self.ctx.serie("nivel_t1_pct"), [(T0, 80.0)])
        self.assertEqual(self.ctx.serie("vol_t1_l"), [(T0, 800)])

    def test_los_nombres_viejos_del_codec_no_se_guardan(self):
        r = procesar(self.conector, self.ctx, msg({"distancia_t1": 50.5, "porcentaje_t1": 80, "litros_t1": 800}))
        self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_campos_validos"))

    def test_nivel_bajo_una_vez_por_cruce(self):
        for i, nivel in enumerate([50, 19, 15, 10, 30, 40]):
            procesar(self.conector, self.ctx, tanques(nivel, 60, i))
        self.assertEqual([(a.tipo, a.severidad) for a in self.ctx.alertas],
                         [("nivel_bajo", "warning"), ("nivel_bajo", "info")])
        self.assertIn("Tanque 1", self.ctx.alertas[0].mensaje)

    def test_cada_tanque_alerta_por_separado(self):
        procesar(self.conector, self.ctx, tanques(50, 50, 0))
        procesar(self.conector, self.ctx, tanques(10, 50, 1))
        procesar(self.conector, self.ctx, tanques(10, 5, 2))
        self.assertEqual([a.mensaje.split(" al ")[0] for a in self.ctx.alertas], ["Tanque 1", "Tanque 2"])

    def test_bajo_desde_la_primera_medicion_alerta(self):
        procesar(self.conector, self.ctx, tanques(5, 60))
        self.assertEqual([(a.tipo, a.severidad) for a in self.ctx.alertas], [("nivel_bajo", "warning")])

    def test_nivel_alto_una_vez_por_cruce(self):
        for i, nivel in enumerate([80, 95, 98, 100, 90, 70]):
            procesar(self.conector, self.ctx, tanques(nivel, 60, i))
        self.assertEqual([(a.tipo, a.severidad) for a in self.ctx.alertas],
                         [("nivel_alto", "warning"), ("nivel_alto", "info")])
        self.assertIn("Tanque 1 lleno, al 95.0 %", self.ctx.alertas[0].mensaje)

    def test_zona_ciega_avisa_rebalse_o_sensor_obstruido(self):
        # mas cerca que la marca de lleno (40 cm): el agua por encima, o condensacion en el sensor
        procesar(self.conector, self.ctx, msg({"dist_t1_cm": 12.0, "dist_t2_cm": 110.0}))
        self.assertEqual(self.ctx.serie("nivel_t1_pct"), [(T0, 100.0)])
        self.assertEqual([(a.tipo, a.severidad) for a in self.ctx.alertas], [("nivel_alto", "warning")])
        self.assertIn("obstruido", self.ctx.alertas[0].mensaje)
        self.assertIn("12.0 cm", self.ctx.alertas[0].mensaje)

    def test_bajo_y_alto_en_tanques_distintos(self):
        procesar(self.conector, self.ctx, tanques(10, 97))
        self.assertEqual(sorted((a.tipo, a.mensaje.split(" ")[1]) for a in self.ctx.alertas),
                         [("nivel_alto", "2"), ("nivel_bajo", "1")])


if __name__ == "__main__":
    unittest.main()
