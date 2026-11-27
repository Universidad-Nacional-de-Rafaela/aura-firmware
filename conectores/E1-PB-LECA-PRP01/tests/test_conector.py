"""Tests del conector. Correr desde la raíz del repo:

    python3 -m unittest discover -s conectores/E1-PB-LECA-PRP01/tests
"""
import sys
import unittest
from datetime import datetime, timezone
from pathlib import Path

CARPETA = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CARPETA.parent.parent / "comun" / "python"))   # aura_sdk
from aura_sdk import ContextoDePrueba, Mensaje, cargar_conector, procesar  # noqa: E402


def msg(values, ingest_id="id-1"):
    return Mensaje(hw_id="mac-dca632aabbcc", values=values, ingest_id=ingest_id,
                   ts=datetime(2026, 11, 16, 12, 0, tzinfo=timezone.utc))


class Conector(unittest.TestCase):
    def setUp(self):
        self.manifiesto, self.conector = cargar_conector(CARPETA)
        self.ctx = ContextoDePrueba(self.manifiesto)

    def ultima(self):
        return self.ctx.mediciones[-1].values

    def test_hay_personas_se_guarda_como_1(self):
        r = procesar(self.conector, self.ctx, msg({"presencia": 1, "conf_pct": 72}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ultima(), {"presencia": 1, "conf_pct": 72})

    def test_sala_vacia_se_guarda_como_0(self):
        r = procesar(self.conector, self.ctx, msg({"presencia": 0}))
        self.assertEqual(r.resultado, "persistido")
        self.assertEqual(self.ultima(), {"presencia": 0})

    def test_confianza_en_los_limites_se_conserva(self):
        for confianza in (0, 100):
            with self.subTest(confianza=confianza):
                r = procesar(self.conector, self.ctx,
                             msg({"presencia": 1, "conf_pct": confianza}, f"limite-{confianza}"))
                self.assertEqual(r.resultado, "persistido")
                self.assertEqual(self.ultima(), {"presencia": 1, "conf_pct": confianza})

    def test_confianza_invalida_se_quita_pero_la_presencia_queda(self):
        for i, confianza in enumerate((-1, 150, True, False, "72", None, float("nan"))):
            with self.subTest(confianza=confianza):
                r = procesar(self.conector, self.ctx,
                             msg({"presencia": 1, "conf_pct": confianza}, f"confianza-{i}"))
                self.assertEqual(r.resultado, "persistido")
                self.assertEqual(r.quitados, ["conf_pct"])
                self.assertEqual(self.ultima(), {"presencia": 1})

    def test_presencia_invalida_se_descarta(self):
        for i, valor in enumerate((True, False, 0.0, 1.0, -1, 2, "0", "1", "si", 0.5, None)):
            with self.subTest(valor=valor):
                r = procesar(self.conector, self.ctx, msg({"presencia": valor}, f"invalida-{i}"))
                self.assertEqual(r.resultado, "descartado")
                self.assertEqual(r.motivo, "presencia_invalida")
                self.assertEqual(self.ctx.mediciones, [])

    def test_sin_presencia_se_descarta_y_no_se_inventa_un_cero(self):
        for i, values in enumerate(({}, {"conf_pct": 80})):
            with self.subTest(values=values):
                r = procesar(self.conector, self.ctx, msg(values, f"sin-presencia-{i}"))
                self.assertEqual(r.resultado, "descartado")
                self.assertEqual(r.motivo, "sin_presencia")
                self.assertEqual(self.ctx.mediciones, [])

    def test_descarte_tras_presencia_no_agrega_una_ausencia(self):
        procesar(self.conector, self.ctx, msg({"presencia": 1}, "valida"))
        r = procesar(self.conector, self.ctx, msg({}, "sin-dato"))
        self.assertEqual(r.resultado, "descartado")
        self.assertEqual(len(self.ctx.mediciones), 1)
        self.assertEqual(self.ultima(), {"presencia": 1})
        self.assertEqual(self.ctx.alertas, [])

    def test_fecha_e_identificador_de_ingesta_se_conservan(self):
        entrada = msg({"presencia": 1}, "original")
        procesar(self.conector, self.ctx, entrada)
        guardada = self.ctx.mediciones[-1]
        self.assertEqual(guardada.ts, entrada.ts)
        self.assertEqual(guardada.ingest_id, entrada.ingest_id)


if __name__ == "__main__":
    unittest.main()
