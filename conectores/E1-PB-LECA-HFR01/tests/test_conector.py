import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

CARPETA = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(CARPETA.parent.parent / "comun" / "python"))
from aura_sdk import ContextoDePrueba, Mensaje, cargar_conector, procesar

T = datetime(2026, 10, 8, 12, tzinfo=timezone.utc)
H, F = "temp_heladera_c", "temp_freezer_c"


class TestConector(unittest.TestCase):
    def setUp(self):
        self.manifiesto, self.conector = cargar_conector(CARPETA)
        self.ctx = ContextoDePrueba(self.manifiesto)
        self.n = 0

    def enviar(self, values, ts=T, ingest_id=None):
        self.n += 1
        msg = Mensaje(hw_id="mac-000000000001", values=values, ts=ts, ingest_id=ingest_id or f"prueba-{self.n}")
        return procesar(self.conector, self.ctx, msg)

    def test_guarda_dos_sondas(self):
        self.assertEqual(self.enviar({H:4.5,F:-18}).resultado,"persistido")
        self.assertEqual(self.ctx.mediciones[0].values,{H:4.5,F:-18})

    def test_intervalos_independientes_no_completan_campo_ausente(self):
        self.enviar({H:4});self.enviar({F:-19})
        self.assertEqual(self.ctx.mediciones[1].values,{F:-19})

    def test_tipos_centienelas_y_rangos_invalidos(self):
        for valor in [-127,85,126,-56,float("nan"),float("inf"),10**400,True,"4",None]:
            with self.subTest(valor=valor):
                self.assertEqual(self.enviar({H:valor}).resultado,"descartado")
        self.assertEqual(len(self.ctx.mediciones),0)

    def test_validacion_independiente(self):
        self.enviar({H:85,F:-18,"rssi":-99})
        self.assertEqual(self.ctx.mediciones[0].values,{F:-18})

    def test_valores_fisicos_en_extremos(self):
        self.assertEqual(self.enviar({H:-55,F:125}).resultado,"persistido")

    def test_sin_campos_validos(self):
        for values in [{},{"rssi":-80},{"temp_c":3}]:
            self.assertEqual(self.enviar(values).resultado,"descartado")

    def test_no_inventa_ts_de_medicion(self):
        self.enviar({H:4},ts=None)
        self.assertIsNotNone(self.ctx.mediciones[0].ts)  # llegada asignada por el SDK

    def test_conserva_hora_original(self):
        self.enviar({H:4},ts=T-timedelta(hours=3))
        self.assertEqual(self.ctx.mediciones[0].ts,T-timedelta(hours=3))

    def test_duplicado_no_guarda_ni_alerta_otro(self):
        self.enviar({H:10},ingest_id="igual")
        self.assertEqual(self.enviar({H:10},ingest_id="igual").resultado,"duplicado")
        self.assertEqual(len(self.ctx.mediciones),1);self.assertEqual(len(self.ctx.alertas),1)

    def test_alarma_una_por_cruce_y_recuperacion(self):
        for n, temp in enumerate([4,8,9,6,4]):
            self.enviar({H:temp},ts=T+timedelta(minutes=n))
        self.assertEqual([a.severidad for a in self.ctx.alertas],["warning","info"])

    def test_alarma_baja_freezer(self):
        self.enviar({F:-26});self.enviar({F:-27},T+timedelta(minutes=1));self.enviar({F:-18},T+timedelta(minutes=2))
        self.assertEqual([a.tipo for a in self.ctx.alertas],["temperatura_freezer"]*2)

    def test_ambas_alarmas_independientes(self):
        self.enviar({H:10,F:-10})
        self.assertEqual({a.tipo for a in self.ctx.alertas},{"temperatura_heladera","temperatura_freezer"})

    def test_respeta_umbrales_reportados(self):
        self.ctx = ContextoDePrueba(self.manifiesto, config={"hlo":0,"hhi":10})
        self.enviar({H:8});self.assertEqual(self.ctx.alertas,[])
        self.enviar({H:11},T+timedelta(minutes=1));self.assertEqual(len(self.ctx.alertas),1)

    def test_config_invalida_usa_defaults(self):
        self.ctx = ContextoDePrueba(self.manifiesto, config={"hlo":9,"hhi":2})
        self.enviar({H:8});self.assertEqual(len(self.ctx.alertas),1)

    def test_recrear_conector_no_repite_alarma(self):
        self.enviar({H:8})
        _, self.conector = cargar_conector(CARPETA)
        self.enviar({H:9},T+timedelta(minutes=1));self.assertEqual(len(self.ctx.alertas),1)

    def test_dato_antiguo_no_simula_recuperacion(self):
        self.enviar({H:8},T)
        self.enviar({H:4},T-timedelta(hours=1))
        self.assertEqual(len(self.ctx.mediciones),2);self.assertEqual(len(self.ctx.alertas),1)

    def test_dato_antiguo_no_simula_alarma_actual(self):
        self.enviar({H:4},T);self.enviar({H:8},T-timedelta(hours=1))
        self.assertEqual(self.ctx.alertas,[])

    def test_cambio_de_baja_a_alta(self):
        self.enviar({H:0},T);self.enviar({H:10},T+timedelta(minutes=1))
        self.assertEqual(len(self.ctx.alertas),2)

    def test_limites_de_conservacion_inclusivos(self):
        self.enviar({H:2,F:-25});self.enviar({H:6,F:-15},T+timedelta(minutes=1))
        self.assertEqual(self.ctx.alertas,[])

    def test_no_prediccion_opcional(self):
        self.assertIsNone(self.manifiesto.prediccion)


if __name__ == "__main__":
    unittest.main()
