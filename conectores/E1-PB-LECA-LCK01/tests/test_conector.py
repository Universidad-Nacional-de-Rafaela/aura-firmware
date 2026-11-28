"""Tests del conector. Correr desde la raíz del repo:

    python3 -m unittest discover -s conectores/E1-PB-LECA-LCK01/tests
"""
import json
import sys
import unittest
from datetime import datetime, timezone
from pathlib import Path

CARPETA = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CARPETA.parent.parent / "comun" / "python"))   # aura_sdk
from aura_sdk import ContextoDePrueba, Mensaje, cargar_conector, procesar  # noqa: E402

CERRADO = {f"puerta_{i}": 0 for i in range(1, 5)} | {f"cerradura_{i}": 0 for i in range(1, 5)}


def msg(values, ingest_id="id-1"):
    return Mensaje(hw_id="mac-000000000000", values=values, ingest_id=ingest_id,
                   ts=datetime(2026, 11, 26, 18, 0, tzinfo=timezone.utc))


class Conector(unittest.TestCase):
    def setUp(self):
        self.manifiesto, self.conector = cargar_conector(CARPETA)
        self.ctx = ContextoDePrueba(self.manifiesto)

    def test_declara_los_ocho_campos(self):
        r = procesar(self.conector, self.ctx, msg(CERRADO))
        self.assertEqual(r.resultado, "persistido")
        for campo in CERRADO:
            self.assertEqual([v for _, v in self.ctx.serie(campo)], [0], campo)

    def test_guarda_cada_locker_por_separado(self):
        r = procesar(self.conector, self.ctx, msg(CERRADO | {"puerta_3": 1, "cerradura_3": 1}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual([v for _, v in self.ctx.serie("puerta_3")], [1])
        self.assertEqual([v for _, v in self.ctx.serie("cerradura_3")], [1])
        self.assertEqual([v for _, v in self.ctx.serie("puerta_1")], [0])

    def test_un_valor_fuera_de_rango_no_se_guarda(self):
        r = procesar(self.conector, self.ctx, msg({"puerta_1": 2, "cerradura_1": 0}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("puerta_1"), [])
        self.assertEqual([v for _, v in self.ctx.serie("cerradura_1")], [0])

    def test_un_valor_intermedio_no_es_un_estado(self):
        r = procesar(self.conector, self.ctx, msg({"puerta_2": 0.5, "cerradura_2": 0}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ctx.serie("puerta_2"), [])

    def test_sin_ningun_estado_valido_se_descarta(self):
        r = procesar(self.conector, self.ctx, msg({"puerta_1": 0.5, "cerradura_1": 7}))
        self.assertEqual(r.resultado, "descartado")

    def test_un_locker_que_no_existe_no_se_guarda(self):
        r = procesar(self.conector, self.ctx, msg({"puerta_1": 0, "puerta_5": 1}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual([v for _, v in self.ctx.serie("puerta_1")], [0])
        with self.assertRaises(KeyError):              # puerta_5 no está en el manifiesto
            self.ctx.serie("puerta_5")

    def test_una_apertura_completa_del_locker_2(self):
        """Cerrado, pulso, abierto (corte al abrir), cerrado: se guardan los estados en orden."""
        with open(CARPETA / "tests" / "mensajes_ejemplo.jsonl", encoding="utf-8") as f:
            for linea in f:
                d = json.loads(linea)
                ts = datetime.fromisoformat(d["ts"].replace("Z", "+00:00"))
                r = procesar(self.conector, self.ctx,
                             Mensaje(hw_id="mac-000000000000", values=d["values"],
                                     ingest_id=d["ingest_id"], ts=ts))
                self.assertEqual(r.resultado, "persistido")
        self.assertEqual([v for _, v in self.ctx.serie("puerta_2")], [0, 0, 1, 0])
        self.assertEqual([v for _, v in self.ctx.serie("cerradura_2")], [0, 1, 0, 0])
        self.assertEqual([v for _, v in self.ctx.serie("puerta_1")], [0, 0, 0, 0])


if __name__ == "__main__":
    unittest.main()
