"""Conector de E1-PB-LECA-LCK01 — gabinete de lockers del pañol (LabECA).

Una placa, 4 lockers. Guarda el estado de la puerta de cada locker (microswitch, puerta_1 a
puerta_4) y de su cerradura (energizada o no, cerradura_1 a cerradura_4). Son estados, no
magnitudes: solo valen 0 y 1. Un valor intermedio (0.5) entra en el rango del manifiesto pero no
es un estado, así que se quita antes de guardar.

Las alertas (intrusion, arranque_puerta_abierta, puerta_abierta, puerta_cerrada, limite_pulsos,
con details.puerta) las emite la placa (contrato §3.5): el conector no las repite.
"""
from aura_sdk import Conector


class ConectorGabinete(Conector):
    def al_recibir_datos(self, msg, ctx):
        estados = {campo: valor for campo, valor in msg.values.items()
                   if not isinstance(valor, bool) and valor in (0, 1)}
        if not estados:
            ctx.descartar("sin_estados_validos")
            return
        ctx.guardar_medicion(estados, msg.ts)
