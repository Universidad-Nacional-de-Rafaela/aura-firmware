"""Tests del conector de E1-PB-LECA-LUZ01, sin AURA ni placa. Correr desde la raíz del repo:

    python3 -m unittest discover -s conectores/E1-PB-LECA-LUZ01/tests
"""
import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

CARPETA = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CARPETA.parent.parent / "comun" / "python"))   # aura_sdk
from aura_sdk import ContextoDePrueba, Mensaje, cargar_conector, procesar  # noqa: E402

T0 = datetime(2026, 10, 8, 12, 0, tzinfo=timezone.utc)


def msg(presencia, minuto=0, n=None):
    return Mensaje(hw_id="mac-e072a1f7efe4", values={"presencia": presencia},
                   ts=T0 + timedelta(minutes=minuto),
                   ingest_id=f"id-{n if n is not None else minuto}")


def msg_completo(minuto=0, n=None):
    """Una muestra como la manda el firmware con todo el hardware conectado."""
    return Mensaje(hw_id="mac-e072a1f7efe4",
                   values={"presencia": 1, "luz_ventana_lx": 250.5, "luz_aula_lx": 180.0,
                           "temp_c": 22.4, "humedad_pct": 55.0, "rele_canal1": 1},
                   ts=T0 + timedelta(minutes=minuto),
                   ingest_id=f"id-c{n if n is not None else minuto}")


class ConectorLuz01(unittest.TestCase):
    def setUp(self):
        self.manifiesto, self.conector = cargar_conector(CARPETA)
        self.ctx = ContextoDePrueba(self.manifiesto)

    def test_guarda_presencia_valida(self):
        r = procesar(self.conector, self.ctx, msg(1, 0))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("presencia"), [(T0, 1)])

    def test_guarda_ausencia_como_cero(self):
        r = procesar(self.conector, self.ctx, msg(0, 0))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("presencia"), [(T0, 0)])

    def test_un_valor_fuera_de_rango_no_se_guarda(self):
        r = procesar(self.conector, self.ctx, msg(2, 0))
        self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_campos_validos"))
        self.assertEqual(self.ctx.mediciones, [])

    def test_campo_no_declarado_no_se_guarda(self):
        r = procesar(self.conector, self.ctx,
                     Mensaje(hw_id="mac-e072a1f7efe4", values={"viento_mps": 3.2},
                             ts=T0, ingest_id="id-x"))
        self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_campos_validos"))

    # === Etapa 2: luz de los BH1750 y estado del relé ===

    def test_guarda_todos_los_campos_del_firmware(self):
        r = procesar(self.conector, self.ctx, msg_completo(0))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("presencia"), [(T0, 1)])
        self.assertEqual(self.ctx.serie("luz_ventana_lx"), [(T0, 250.5)])
        self.assertEqual(self.ctx.serie("luz_aula_lx"), [(T0, 180.0)])
        self.assertEqual(self.ctx.serie("temp_c"), [(T0, 22.4)])
        self.assertEqual(self.ctx.serie("humedad_pct"), [(T0, 55.0)])
        self.assertEqual(self.ctx.serie("rele_canal1"), [(T0, 1)])
        # solo la alerta de presencia: la luz, el clima y el relé no generan alertas
        self.assertEqual([a.mensaje for a in self.ctx.alertas], ["Aula ocupada"])

    def test_un_lux_fuera_de_rango_no_tira_la_muestra(self):
        r = procesar(self.conector, self.ctx,
                     Mensaje(hw_id="mac-e072a1f7efe4",
                             values={"presencia": 1, "luz_aula_lx": 999999},
                             ts=T0, ingest_id="id-y"))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("presencia"), [(T0, 1)])
        self.assertEqual(self.ctx.serie("luz_aula_lx"), [])   # lo quitó el rango

    def test_muestra_sin_presencia_tambien_se_guarda(self):
        # el firmware siempre manda presencia junto, pero si solo llegara luz
        # hay que guardarla igual (y no alerta: no es un cruce de presencia)
        r = procesar(self.conector, self.ctx,
                     Mensaje(hw_id="mac-e072a1f7efe4", values={"luz_ventana_lx": 120.0},
                             ts=T0, ingest_id="id-z"))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("luz_ventana_lx"), [(T0, 120.0)])
        self.assertEqual(self.ctx.alertas, [])

    def test_alerta_una_vez_por_cruce(self):
        for i, p in enumerate([0, 1, 1, 1, 0, 0, 1]):
            procesar(self.conector, self.ctx, msg(p, i))
        self.assertEqual([(a.tipo, a.severidad, a.mensaje) for a in self.ctx.alertas],
                         [("presencia", "info", "Aula ocupada"),
                          ("presencia", "info", "Aula libre"),
                          ("presencia", "info", "Aula ocupada")])

    def test_primer_dato_ocupado_alerta_una_vez(self):
        procesar(self.conector, self.ctx, msg(1, 0))
        procesar(self.conector, self.ctx, msg(1, 1))
        self.assertEqual([a.mensaje for a in self.ctx.alertas], ["Aula ocupada"])

    def test_primer_dato_libre_no_alerta(self):
        procesar(self.conector, self.ctx, msg(0, 0))
        self.assertEqual(self.ctx.alertas, [])   # no había estado anterior: no es un cruce

    def test_ingest_id_repetido_es_duplicado(self):
        self.assertEqual(procesar(self.conector, self.ctx, msg(1, 0, n="unico")).resultado,
                         "persistido")
        r = procesar(self.conector, self.ctx, msg(0, 5, n="unico"))
        self.assertEqual(r.resultado, "duplicado")
        self.assertEqual(len(self.ctx.alertas), 1)   # el duplicado no agrega nada (quedó la del 1.º)

    def test_excepcion_no_guarda_nada_y_no_confirma(self):
        def explotar(m, c):
            raise RuntimeError("fallo de verdad")
        self.conector.al_recibir_datos = explotar
        r = procesar(self.conector, self.ctx, msg(1, 0))
        self.assertIsNone(r.resultado)   # sin ack: el dispositivo reenvía
        self.assertEqual(self.ctx.mediciones, [])
        self.assertEqual(self.ctx.alertas, [])

    def test_sin_accion_se_descarta(self):
        self.conector.al_recibir_datos = lambda m, c: None
        r = procesar(self.conector, self.ctx, msg(1, 0))
        self.assertEqual((r.resultado, r.motivo), ("descartado", "sin_accion"))


if __name__ == "__main__":
    unittest.main()
