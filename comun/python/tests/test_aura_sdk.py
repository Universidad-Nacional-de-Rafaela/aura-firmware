"""Tests de aura_sdk. Correr: python3 -m unittest discover -s comun/python/tests"""
import sys
import tempfile
import textwrap
import unittest
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import aura_sdk  # noqa: E402
from aura_sdk import (  # noqa: E402
    Conector, ContextoDePrueba, ManifiestoInvalido, Mensaje, cargar_conector,
    cargar_manifiesto, procesar,
)
from aura_sdk.contrato import PruebasDeContrato  # noqa: E402

MANIFIESTO = """
nombre  = "{nombre}"
version = "0.1.0"
sdk     = "{sdk}"
autores = ["catedra"]
clase   = "conector:ConectorPrueba"

[campos.temp_c]
unidad = "°C"
min = -40
max = 125

[campos.hum_pct]
unidad = "%"
min = 0
max = 100
{extra}
"""

CODIGO = """
from aura_sdk import Conector

class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        ctx.guardar_medicion(msg.values, msg.ts)
"""


def carpeta(tmp, nombre="E1-PB-LECA-TEM01", sdk=">=1,<2", extra="", codigo=CODIGO):
    d = Path(tmp) / nombre
    d.mkdir()
    (d / "conector.toml").write_text(textwrap.dedent(MANIFIESTO.format(nombre=nombre, sdk=sdk, extra=extra)),
                                     encoding="utf-8")
    (d / "conector.py").write_text(textwrap.dedent(codigo), encoding="utf-8")
    return d


def mensaje(values, ts=None, ingest_id="5f0c1b1e-8a6d-4a55-9f2b-7c3e2d1a0b99"):
    return Mensaje(hw_id="mac-e072a1f7efe4", values=values, ts=ts, ingest_id=ingest_id,
                   adaptador="mac-e072a1d848b0")


class Manifiesto(unittest.TestCase):
    def test_valido(self):
        with tempfile.TemporaryDirectory() as tmp:
            m = cargar_manifiesto(carpeta(tmp, extra='[prediccion]\ncada = "15m"\nhorizonte = "6h"\n'))
            self.assertEqual(m.nombre, "E1-PB-LECA-TEM01")
            self.assertEqual(m.campos["temp_c"].min, -40)
            self.assertEqual(m.prediccion.cada_s, 900)
            self.assertEqual(m.prediccion.horizonte_s, 6 * 3600)

    def test_nombre_distinto_de_la_carpeta(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = carpeta(tmp)
            (d / "conector.toml").write_text((d / "conector.toml").read_text().replace(
                'nombre  = "E1-PB-LECA-TEM01"', 'nombre  = "otro"'), encoding="utf-8")
            with self.assertRaisesRegex(ManifiestoInvalido, "nombre"):
                cargar_manifiesto(d)

    def test_sdk_que_no_se_cumple(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ManifiestoInvalido, "sdk"):
                cargar_manifiesto(carpeta(tmp, sdk=">=2,<3"))

    def test_sdk_mal_escrito(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ManifiestoInvalido, "sdk"):
                cargar_manifiesto(carpeta(tmp, sdk="uno"))

    def test_rango_invertido(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = carpeta(tmp)
            (d / "conector.toml").write_text((d / "conector.toml").read_text().replace("max = 100", "max = -5"),
                                             encoding="utf-8")
            with self.assertRaisesRegex(ManifiestoInvalido, "hum_pct"):
                cargar_manifiesto(d)

    def test_campo_sin_unidad(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = carpeta(tmp)
            (d / "conector.toml").write_text((d / "conector.toml").read_text().replace('unidad = "%"\n', ""),
                                             encoding="utf-8")
            with self.assertRaisesRegex(ManifiestoInvalido, "unidad"):
                cargar_manifiesto(d)

    def test_nombre_de_campo_invalido(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = carpeta(tmp)
            (d / "conector.toml").write_text((d / "conector.toml").read_text().replace("campos.temp_c", "campos.Temp"),
                                             encoding="utf-8")
            with self.assertRaisesRegex(ManifiestoInvalido, "Temp"):
                cargar_manifiesto(d)

    def test_duracion_mal_escrita(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ManifiestoInvalido, "cada"):
                cargar_manifiesto(carpeta(tmp, extra='[prediccion]\ncada = "15 minutos"\nhorizonte = "6h"\n'))

    def test_toml_roto(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = carpeta(tmp)
            (d / "conector.toml").write_text("nombre = ", encoding="utf-8")
            with self.assertRaises(ManifiestoInvalido):
                cargar_manifiesto(d)


class Carga(unittest.TestCase):
    def test_carga_la_clase(self):
        with tempfile.TemporaryDirectory() as tmp:
            m, c = cargar_conector(carpeta(tmp))
            self.assertIsInstance(c, Conector)
            self.assertIs(c.manifiesto, m)

    def test_clase_que_no_hereda(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = carpeta(tmp, codigo="class ConectorPrueba:\n    pass\n")
            with self.assertRaisesRegex(ManifiestoInvalido, "Conector"):
                cargar_conector(d)

    def test_sin_al_recibir_datos(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = carpeta(tmp, codigo="from aura_sdk import Conector\nclass ConectorPrueba(Conector):\n    pass\n")
            with self.assertRaisesRegex(ManifiestoInvalido, "al_recibir_datos"):
                cargar_conector(d)

    def test_dos_conectores_con_el_mismo_modulo_no_se_pisan(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, a = cargar_conector(carpeta(tmp, nombre="E1-PB-LECA-TEM01"))
            otro = CODIGO.replace("ctx.guardar_medicion(msg.values, msg.ts)", "ctx.descartar('b')")
            _, b = cargar_conector(carpeta(tmp, nombre="E1-PB-LECA-TEM02", codigo=otro))
            self.assertIsNot(type(a), type(b))


class Procesar(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m, self.c = cargar_conector(carpeta(self.tmp.name))
        self.ctx = ContextoDePrueba(self.m)

    def tearDown(self):
        self.tmp.cleanup()

    def test_guarda_y_queda_persistido(self):
        r = procesar(self.c, self.ctx, mensaje({"temp_c": 4.5}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.mediciones[0].values, {"temp_c": 4.5})

    def test_campo_fuera_de_rango_se_quita(self):
        r = procesar(self.c, self.ctx, mensaje({"temp_c": 4.5, "hum_pct": 140}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.mediciones[0].values, {"temp_c": 4.5})
        self.assertIn("hum_pct", r.quitados)

    def test_campo_no_declarado_se_quita(self):
        r = procesar(self.c, self.ctx, mensaje({"temp_c": 4.5, "rssi": -80}))
        self.assertEqual(self.ctx.mediciones[0].values, {"temp_c": 4.5})
        self.assertIn("rssi", r.quitados)

    def test_valor_no_numerico_se_quita(self):
        procesar(self.c, self.ctx, mensaje({"temp_c": "4.5", "hum_pct": True, "temp_c2": None}))
        self.assertEqual(self.ctx.mediciones, [])

    def test_todos_fuera_de_rango_descarta(self):
        r = procesar(self.c, self.ctx, mensaje({"temp_c": 900}))
        self.assertEqual(r.resultado, "descartado")
        self.assertEqual(r.motivo, "sin_campos_validos")
        self.assertEqual(self.ctx.mediciones, [])

    def test_descartar(self):
        c = type("C", (Conector,), {"al_recibir_datos": lambda self, msg, ctx: ctx.descartar("prueba")})(self.m)
        r = procesar(c, self.ctx, mensaje({"temp_c": 1}))
        self.assertEqual((r.resultado, r.motivo), ("descartado", "prueba"))

    def test_sin_accion(self):
        c = type("C", (Conector,), {"al_recibir_datos": lambda self, msg, ctx: None})(self.m)
        r = procesar(c, self.ctx, mensaje({"temp_c": 1}))
        self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_accion"))

    def test_excepcion_queda_sin_ack(self):
        def explota(self, msg, ctx):
            raise ValueError("bug del grupo")
        c = type("C", (Conector,), {"al_recibir_datos": explota})(self.m)
        r = procesar(c, self.ctx, mensaje({"temp_c": 1}))
        self.assertIsNone(r.resultado)
        self.assertIn("bug del grupo", r.error)
        self.assertEqual(self.ctx.mediciones, [])

    def test_lo_guardado_antes_de_una_excepcion_no_queda(self):
        def guarda_y_explota(self, msg, ctx):
            ctx.guardar_medicion(msg.values)
            raise RuntimeError("tarde")
        c = type("C", (Conector,), {"al_recibir_datos": guarda_y_explota})(self.m)
        procesar(c, self.ctx, mensaje({"temp_c": 1}))
        self.assertEqual(self.ctx.mediciones, [])   # rollback, como el runtime

    def test_ingest_id_repetido_es_duplicado_sin_llamar_al_conector(self):
        procesar(self.c, self.ctx, mensaje({"temp_c": 1}))
        llamadas = []
        c = type("C", (Conector,), {"al_recibir_datos": lambda self, msg, ctx: llamadas.append(1)})(self.m)
        r = procesar(c, self.ctx, mensaje({"temp_c": 1}))
        self.assertEqual(r.resultado, "duplicado")
        self.assertEqual(llamadas, [])

    def test_sin_ts_usa_la_hora_de_llegada(self):
        procesar(self.c, self.ctx, mensaje({"temp_c": 1}))
        self.assertIsNotNone(self.ctx.mediciones[0].ts)


class MensajeDesdeData(unittest.TestCase):
    def test_desde_el_payload_de_hw_data(self):
        m = Mensaje.desde_data("mac-e072a1f7efe4", {"values": {"temp_c": 1}, "ingest_id": "x",
                                                    "ts": "2026-10-05T14:03:00Z", "adaptador": "mac-e072a1d848b0"})
        self.assertEqual(m.ts, datetime(2026, 10, 5, 14, 3, tzinfo=timezone.utc))
        self.assertEqual(m.values, {"temp_c": 1})

    def test_values_que_no_es_objeto(self):
        with self.assertRaises(ValueError):
            Mensaje.desde_data("mac-e072a1f7efe4", {"values": "roto"})

    def test_ts_invalido_se_ignora(self):
        m = Mensaje.desde_data("mac-e072a1f7efe4", {"values": {"temp_c": 1}, "ts": "ayer"})
        self.assertIsNone(m.ts)


class ContratoDePrueba(PruebasDeContrato, unittest.TestCase):
    """La suite de contrato contra ContextoDePrueba. El runtime la corre contra el suyo."""

    def crear_contexto(self, manifiesto):
        return ContextoDePrueba(manifiesto)

    def crear_manifiesto(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(tmp))
        return cargar_manifiesto(carpeta(tmp, extra='[prediccion]\ncada = "15m"\nhorizonte = "6h"\n'))


class Version(unittest.TestCase):
    def test_version(self):
        self.assertRegex(aura_sdk.__version__, r"^1\.\d+\.\d+$")


if __name__ == "__main__":
    unittest.main()
