"""Tests de herramientas/correr_conector.py con el conector de ejemplo (sin broker)."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(RAIZ / "herramientas"))
from correr_conector import Corrida  # noqa: E402


class Reloj:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


class CorridaDeEjemplo(unittest.TestCase):
    def setUp(self):
        self.lineas = []
        self.reloj = Reloj()
        self.c = Corrida(RAIZ / "ejemplos" / "conector_ejemplo", "mac-e072a1f7efe4",
                         salida=self.lineas.append, reloj=self.reloj)

    def test_mensaje_valido(self):
        self.c.data({"values": {"temp_chip_c": 35.2}, "ingest_id": "a"})
        self.assertIn("-> persistido", self.lineas[-1])

    def test_values_invalido(self):
        self.c.data({"values": "roto", "ingest_id": "a"})
        self.assertIn("rechazado", self.lineas[-1])

    def test_duplicado(self):
        self.c.data({"values": {"temp_chip_c": 35.2}, "ingest_id": "a"})
        self.c.data({"values": {"temp_chip_c": 35.2}, "ingest_id": "a"})
        self.assertIn("-> duplicado", self.lineas[-1])

    def test_alerta_se_muestra(self):
        self.c.data({"values": {"temp_chip_c": 70}, "ingest_id": "a"})
        self.assertTrue(any("alerta temperatura_alta" in l for l in self.lineas))

    def test_predice_cada_lo_que_dice_el_manifiesto(self):
        self.c.data({"values": {"temp_chip_c": 30}, "ingest_id": "a"})
        self.assertFalse(any("predecir()" in l or "predicción" in l for l in self.lineas))
        self.reloj.t = 15 * 60 + 1
        self.c.data({"values": {"temp_chip_c": 31}, "ingest_id": "b"})
        self.assertTrue(any("predecir() no guardó" in l for l in self.lineas))   # pocos puntos

    def test_falla_del_conector_muestra_conector_fallo(self):
        def lanza(msg, ctx):
            raise ValueError("bug del grupo")
        self.c.conector.al_recibir_datos = lanza
        self.c.data({"values": {"temp_chip_c": 35.2}, "ingest_id": "a"})
        self.assertTrue(any("-> persistido (conector_fallo)" in l for l in self.lineas))
        self.assertTrue(any("ValueError: bug del grupo" in l for l in self.lineas))
        self.assertTrue(any("alerta conector_fallo" in l for l in self.lineas))

    def test_status_actualiza_la_config(self):
        self.c.status({"status": "online", "details": {"config": {"intervalo_s": 30}}})
        self.assertEqual(self.c.ctx.config_vigente(), {"intervalo_s": 30})

    def test_resumen(self):
        self.c.data({"values": {"temp_chip_c": 30}, "ingest_id": "a"})
        self.c.data({"values": {"temp_chip_c": 900}, "ingest_id": "b"})
        self.c.resumen()
        self.assertIn("descartado: 1", self.lineas[-1])
        self.assertIn("persistido: 1", self.lineas[-1])


class Subproceso(unittest.TestCase):
    def test_con_subproceso_da_lo_mismo(self):
        mensajes = "\n".join([
            '{"values": {"temp_chip_c": 35.2}, "ingest_id": "a", "ts": "2026-10-07T12:00:00Z"}',
            '{"values": {"temp_chip_c": 900}, "ingest_id": "b"}',
            '{"values": {"temp_chip_c": 70}, "ingest_id": "c"}',
            '{"values": {"temp_chip_c": 70}, "ingest_id": "c"}',
        ])
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl") as f:
            f.write(mensajes)
            f.flush()
            base = [sys.executable, str(RAIZ / "herramientas" / "correr_conector.py"),
                    str(RAIZ / "ejemplos" / "conector_ejemplo"), "--archivo", f.name]
            sin = subprocess.run(base, capture_output=True, text=True, timeout=60)
            con = subprocess.run(base + ["--subproceso"], capture_output=True, text=True, timeout=60)
        self.assertEqual(sin.returncode, 0, sin.stderr)
        self.assertEqual(con.returncode, 0, con.stderr)
        self.assertIn("persistido: 2", sin.stdout.splitlines()[-1])
        self.assertEqual(con.stdout, sin.stdout)


if __name__ == "__main__":
    unittest.main()
