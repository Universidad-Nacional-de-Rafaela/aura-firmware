"""Tests de aura_sdk.subproceso: el conector corre en un proceso aparte y habla por stdin/stdout.

Cada caso es un conector de prueba en tests/conectores/<caso>/. Correr:
python3 -m unittest discover -s comun/python/tests
"""
import io
import os
import sys
import time
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

SDK_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SDK_DIR))
from aura_sdk import (  # noqa: E402
    ConectorEnSubproceso, ContextoDePrueba, ErrorDelAnfitrion, Medicion, Mensaje, cargar_manifiesto, procesar,
)
from aura_sdk.subproceso import Limites, codificar, decodificar, hijo  # noqa: E402

CASOS = Path(__file__).resolve().parent / "conectores"
UTC = timezone.utc


def correr(caso, *, ctx=None, values=None, ts=None, timeout_s=3, **kw):
    """Pasa una muestra por el conector <caso> en un subproceso. Devuelve (resultado, ctx, conector)."""
    manifiesto = cargar_manifiesto(CASOS / ("guarda" if caso == "manifiesto_roto" else caso))
    ctx = ctx or ContextoDePrueba(manifiesto)
    conector = ConectorEnSubproceso(str(CASOS / caso), str(SDK_DIR), timeout_s=timeout_s, **kw)
    msg = Mensaje(hw_id="mac-e072a1f7efe4", values=values or {"temp_c": 20, "hum_pct": 50},
                  ts=ts or datetime(2026, 10, 23, 12, 0, tzinfo=UTC), ingest_id="i-1", adaptador="mac-raiz")
    return procesar(conector, ctx, msg), ctx, conector


class Codificacion(unittest.TestCase):
    def test_fechas_como_ts(self):
        t = datetime(2026, 10, 1, 12, 30, 15, 123456, tzinfo=UTC)
        linea = codificar({"args": {"ts": t, "lista": [(t, 1.5)]}})
        self.assertNotIn("\n", linea)
        self.assertIn('{"$ts": "2026-10-01T12:30:15.123456+00:00"}', linea)
        self.assertEqual(decodificar(linea), {"args": {"ts": t, "lista": [[t, 1.5]]}})

    def test_fecha_sin_zona_es_utc(self):
        self.assertEqual(decodificar(codificar({"t": datetime(2026, 1, 1)}))["t"], datetime(2026, 1, 1, tzinfo=UTC))

    def test_fecha_en_otra_zona_pasa_a_utc(self):
        t = datetime(2026, 1, 1, 9, tzinfo=timezone(timedelta(hours=-3)))
        self.assertIn("12:00:00+00:00", codificar({"t": t}))

    def test_linea_que_no_es_un_objeto(self):
        for linea in ("[1, 2]", "no es json", ""):
            with self.assertRaises(ValueError):
                decodificar(linea)


class HijoConPadreDePrueba(unittest.TestCase):
    """El runner del hijo, en este proceso, contra respuestas escritas a mano."""

    def test_llamadas_y_fin(self):
        t = datetime(2026, 10, 23, 11, 0, tzinfo=UTC)
        entrada = io.StringIO("".join(codificar(m) + "\n" for m in (
            {"tipo": "inicio", "carpeta": str(CASOS / "guarda"),
             "mensaje": {"hw_id": "mac-1", "values": {"temp_c": 20}, "ts": t, "ingest_id": "i", "adaptador": None}},
            {"tipo": "respuesta", "ok": True, "valor": [[t, 19.5]]},
            {"tipo": "respuesta", "ok": True, "valor": {"temp_c": 20, "hum_pct": 1}},
        )))
        salida = io.StringIO()
        self.assertEqual(hijo(entrada, salida, None), 0)
        enviados = [decodificar(l) for l in salida.getvalue().splitlines()]
        self.assertEqual(enviados, [
            {"tipo": "llamada", "metodo": "serie", "args": {"campo": "temp_c", "desde": None, "hasta": None}},
            {"tipo": "llamada", "metodo": "guardar_medicion", "args": {"values": {"temp_c": 20, "hum_pct": 1}, "ts": t}},
            {"tipo": "fin", "ok": True},
        ])

    def test_respuesta_con_error_se_relanza_con_su_tipo(self):
        entrada = io.StringIO("".join(codificar(m) + "\n" for m in (
            {"tipo": "inicio", "carpeta": str(CASOS / "guarda"),
             "mensaje": {"hw_id": "mac-1", "values": {"temp_c": 20}, "ts": None, "ingest_id": None, "adaptador": None}},
            {"tipo": "respuesta", "ok": False, "error": "ValueError", "detalle": "fuera de rango"},
        )))
        salida = io.StringIO()
        self.assertEqual(hijo(entrada, salida, None), 1)
        fin = decodificar(salida.getvalue().splitlines()[-1])
        self.assertEqual((fin["tipo"], fin["ok"]), ("fin", False))
        self.assertIn("ValueError: fuera de rango", fin["error"])


class EnSubproceso(unittest.TestCase):
    def test_guarda_y_lee_la_serie(self):
        manifiesto = cargar_manifiesto(CASOS / "guarda")
        ctx = ContextoDePrueba(manifiesto)
        ctx.mediciones.append(Medicion(ts=datetime(2026, 10, 23, 11, 0, tzinfo=UTC), values={"temp_c": 19.5}))
        r, ctx, _ = correr("guarda", ctx=ctx)
        self.assertEqual((r.resultado, r.motivo), ("persistido", None), r.error)
        self.assertEqual(ctx.mediciones[-1].values, {"temp_c": 20, "hum_pct": 1})
        self.assertEqual(ctx.mediciones[-1].ingest_id, "i-1")
        self.assertEqual(ctx.alertas, [])

    def test_error_del_padre_se_relanza_con_su_tipo(self):
        r, ctx, _ = correr("error_del_padre")
        self.assertEqual((r.resultado, r.motivo), ("persistido", None), r.error)

    def test_print_no_rompe_el_protocolo(self):
        r, ctx, conector = correr("con_print")
        self.assertEqual((r.resultado, r.motivo), ("persistido", None), r.error)
        self.assertEqual(conector.stderr.count("hola"), 2)

    def test_excepcion_del_conector(self):
        r, ctx, _ = correr("excepcion")
        self.assertEqual((r.resultado, r.motivo), ("persistido", "conector_fallo"))
        self.assertIn("ValueError: bug del grupo", r.error)
        self.assertIn("Traceback", r.error)
        self.assertEqual([(a.tipo, a.severidad) for a in ctx.alertas], [("conector_fallo", "high")])
        self.assertIn("(excepcion)", ctx.alertas[0].mensaje)
        self.assertEqual(ctx.mediciones[-1].values, {"temp_c": 20, "hum_pct": 50})   # la muestra cruda

    def test_timeout(self):
        t0 = time.monotonic()
        r, ctx, _ = correr("colgado", timeout_s=1)
        self.assertLess(time.monotonic() - t0, 3)
        self.assertEqual(r.motivo, "conector_fallo")
        self.assertIn("(timeout)", ctx.alertas[0].mensaje)

    def test_timeout_cuenta_desde_el_inicio(self):
        t0 = time.monotonic()
        r, ctx, _ = correr("colgado_tras_serie", timeout_s=1)
        self.assertLess(time.monotonic() - t0, 3)
        self.assertIn("(timeout)", ctx.alertas[0].mensaje)

    def test_limite_de_cpu(self):
        r, ctx, _ = correr("colgado", timeout_s=3, limites=Limites(cpu_s=1))
        self.assertIn("(timeout)", ctx.alertas[0].mensaje)
        self.assertIn("CPU", r.error)

    def test_memoria(self):
        r, ctx, _ = correr("memoria")
        self.assertEqual(r.motivo, "conector_fallo")
        self.assertIn("(memoria)", ctx.alertas[0].mensaje)
        self.assertIn("MemoryError", r.error)

    def test_murio_sin_mandar_fin(self):
        r, ctx, _ = correr("muere")
        self.assertIn("(murio)", ctx.alertas[0].mensaje)
        self.assertIn("3", r.error)

    def test_linea_que_no_es_json(self):
        r, ctx, _ = correr("no_json")
        self.assertEqual((r.resultado, r.motivo), ("persistido", "conector_fallo"))
        self.assertIn("(protocolo)", ctx.alertas[0].mensaje)

    def test_tipo_desconocido(self):
        r, ctx, _ = correr("tipo_desconocido")
        self.assertIn("(protocolo)", ctx.alertas[0].mensaje)

    def test_manifiesto_roto(self):
        r, ctx, _ = correr("manifiesto_roto")
        self.assertEqual(r.motivo, "conector_fallo")
        self.assertIn("(manifiesto)", ctx.alertas[0].mensaje)

    def test_fechas_viajan_como_datetime(self):
        r, ctx, _ = correr("fechas")
        self.assertEqual(r.resultado, "persistido", r.error)
        self.assertEqual(ctx.mediciones[-1].ts, datetime(2026, 10, 1, 12, 30, 15, 123456, tzinfo=UTC))

    def test_error_del_anfitrion_no_es_falla_del_conector(self):
        class BaseCaida(ContextoDePrueba):
            def serie(self, campo, desde=None, hasta=None):
                raise ConnectionError("la base no contesta")

        ctx = BaseCaida(cargar_manifiesto(CASOS / "guarda"))
        with self.assertRaises(ConnectionError):
            correr("guarda", ctx=ctx)
        self.assertEqual(ctx.alertas, [])
        self.assertEqual(ctx.mediciones, [])
        self.assertIsNone(ctx._msg)

    def test_error_del_anfitrion_guarda_el_original(self):
        e = ErrorDelAnfitrion(ConnectionError("x"))
        self.assertIsInstance(e.original, ConnectionError)

    @unittest.skipIf(os.geteuid() == 0, "como root sí puede cambiar de usuario")
    def test_sin_permisos_para_cambiar_de_usuario(self):
        r, ctx, _ = correr("guarda", popen={"user": 65534})
        self.assertEqual(r.motivo, "conector_fallo")
        self.assertIn("(sin_permisos)", ctx.alertas[0].mensaje)


if __name__ == "__main__":
    unittest.main()
