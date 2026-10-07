"""Tests de la decisión de herramientas/ack_falso.py (sin broker)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from ack_falso import decidir, hw_id_del_topico  # noqa: E402

ID = "5f0c1b1e-8a6d-4a55-9f2b-7c3e2d1a0b99"


class Decidir(unittest.TestCase):
    def test_primera_vez_persistido_y_despues_duplicado(self):
        vistos = set()
        self.assertEqual(decidir({"values": {"t": 1}, "ingest_id": ID}, "ok", vistos),
                         {"ingest_id": ID, "resultado": "persistido"})
        self.assertEqual(decidir({"values": {"t": 1}, "ingest_id": ID}, "ok", vistos),
                         {"ingest_id": ID, "resultado": "duplicado"})

    def test_sin_ingest_id_no_hay_ack(self):
        self.assertIsNone(decidir({"values": {"t": 1}}, "ok", set()))
        self.assertIsNone(decidir({"values": {"t": 1}, "ingest_id": 5}, "ok", set()))

    def test_values_que_no_es_objeto_se_rechaza(self):
        self.assertEqual(decidir({"values": "{roto", "ingest_id": ID}, "ok", set()),
                         {"ingest_id": ID, "resultado": "rechazado", "motivo": "values_invalido"})

    def test_modo_sin_ack_simula_la_base_caida(self):
        vistos = set()
        self.assertIsNone(decidir({"values": {"t": 1}, "ingest_id": ID}, "sin_ack", vistos))
        self.assertEqual(vistos, set())   # no se guardo: el reintento sale persistido

    def test_modo_cuarentena(self):
        vistos = set()
        self.assertEqual(decidir({"values": {"t": 1}, "ingest_id": ID}, "cuarentena", vistos)["resultado"],
                         "cuarentena")
        self.assertEqual(decidir({"values": {"t": 1}, "ingest_id": ID}, "cuarentena", vistos)["resultado"],
                         "duplicado")

    def test_modo_rechazado(self):
        r = decidir({"values": {"t": 1}, "ingest_id": ID}, "rechazado", set())
        self.assertEqual(r["resultado"], "rechazado")
        self.assertEqual(r["motivo"], "modo_rechazado")


class Topico(unittest.TestCase):
    def test_hw_id(self):
        self.assertEqual(hw_id_del_topico("hw/mac-e072a1f7efe4/data"), "mac-e072a1f7efe4")
        self.assertIsNone(hw_id_del_topico("devices/abc/data"))
        self.assertIsNone(hw_id_del_topico("hw/mac-e072a1f7efe4/status"))
        self.assertIsNone(hw_id_del_topico("hw/MAC-E072A1F7EFE4/data"))


if __name__ == "__main__":
    unittest.main()
