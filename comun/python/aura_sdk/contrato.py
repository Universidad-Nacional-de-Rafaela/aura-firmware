"""Suite de contrato del Contexto.

La corren ContextoDePrueba (acá) y el Contexto real del runtime de AURA (en aura-app), para que
un conector que pasa sus tests en el banco se comporte igual en AURA. Uso:

    class MiContrato(PruebasDeContrato, unittest.TestCase):
        def crear_manifiesto(self): ...           # un manifiesto con campos temp_c y hum_pct
        def crear_contexto(self, manifiesto): ...
"""
from datetime import datetime, timedelta, timezone

from .ejecutar import procesar
from .mensaje import Mensaje


class _Guarda:
    def __init__(self, values, ts=None):
        self.values, self.ts = values, ts

    def al_recibir_datos(self, msg, ctx):
        ctx.guardar_medicion(self.values, self.ts)


class PruebasDeContrato:
    def crear_manifiesto(self):
        raise NotImplementedError

    def crear_contexto(self, manifiesto):
        raise NotImplementedError

    def _ctx(self):
        return self.crear_contexto(self.crear_manifiesto())

    def _guardar(self, ctx, values, ts=None, ingest_id=None):
        msg = Mensaje(hw_id="mac-e072a1f7efe4", values=values, ts=ts, ingest_id=ingest_id)
        return procesar(_Guarda(values, ts), ctx, msg)

    def test_contrato_guarda_y_lee_la_serie_en_orden(self):
        ctx = self._ctx()
        t0 = datetime(2026, 10, 7, 12, 0, tzinfo=timezone.utc)
        self._guardar(ctx, {"temp_c": 5.0}, t0 + timedelta(minutes=2), "b")
        self._guardar(ctx, {"temp_c": 4.0}, t0, "a")
        self.assertEqual(ctx.serie("temp_c"), [(t0, 4.0), (t0 + timedelta(minutes=2), 5.0)])
        self.assertEqual(ctx.serie("temp_c", desde=t0 + timedelta(minutes=1)), [(t0 + timedelta(minutes=2), 5.0)])

    def test_contrato_quita_lo_fuera_de_rango(self):
        ctx = self._ctx()
        r = self._guardar(ctx, {"temp_c": 4.0, "hum_pct": 300}, ingest_id="a")
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(ctx.serie("hum_pct"), [])

    def test_contrato_nada_valido_es_descartado(self):
        r = self._guardar(self._ctx(), {"temp_c": 1000}, ingest_id="a")
        self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_campos_validos"))

    def test_contrato_duplicado(self):
        ctx = self._ctx()
        self._guardar(ctx, {"temp_c": 4.0}, ingest_id="a")
        self.assertEqual(self._guardar(ctx, {"temp_c": 4.0}, ingest_id="a").resultado, "duplicado")
        self.assertEqual(len(ctx.serie("temp_c")), 1)

    def test_contrato_serie_de_un_campo_no_declarado(self):
        with self.assertRaises(KeyError):
            self._ctx().serie("presion_hpa")

    def test_contrato_prediccion_valida(self):
        ctx = self._ctx()
        ctx.guardar_prediccion("temp_c", datetime(2026, 10, 7, 18, tzinfo=timezone.utc), 4.2, 3.5, 5.0, "lineal")
        with self.assertRaises(ValueError):
            ctx.guardar_prediccion("temp_c", datetime(2026, 10, 7, 18, tzinfo=timezone.utc), 4.2, 5.0, 6.0)
        with self.assertRaises(KeyError):
            ctx.guardar_prediccion("presion_hpa", datetime(2026, 10, 7, 18, tzinfo=timezone.utc), 1.0)

    def test_contrato_alerta(self):
        ctx = self._ctx()
        ctx.alertar("umbral", "high", "Temperatura alta")
        with self.assertRaises(ValueError):
            ctx.alertar("Umbral", "high", "x")
        with self.assertRaises(ValueError):
            ctx.alertar("umbral", "grave", "x")

    def test_contrato_config_es_una_copia(self):
        ctx = self._ctx()
        ctx.config_vigente()["intervalo_s"] = 1
        self.assertNotIn("intervalo_s", ctx.config_vigente())
