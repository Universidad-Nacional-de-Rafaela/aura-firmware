"""Conector de E1-PB-LECA-CRT01 — Cortina automatizada Luflex (BH1750 + NEMA 23 sobre ESP-WIFI-MESH).

La placa manda posicion_pct (0 a 100 %), luz_lux (0 a 54612 lx), estado_motor (0: detenido,
1: moviendo, 2: calibrando) y modo (0: manual, 1: auto1, 2: auto2).

El conector guarda las mediciones dentro de rango y vigila que el motor no quede activo
(moviendo o calibrando) durante más de TIEMPO_MAX_MOTOR_S segundos seguidos sin volver a
detenido, emitiendo una alerta de tipo motor_atascado una sola vez al superar el límite y
cerrándola con severidad info cuando el motor vuelve a detenerse.

No usa atributos de la instancia para recordar estado entre muestras: consulta el historial
con ctx.serie().
"""
from datetime import timedelta

from aura_sdk import Conector

TIPO_ALERTA = "motor_atascado"
TIEMPO_MAX_MOTOR = timedelta(seconds=180)
DETENIDO = 0


def _inicio_activo(serie):
    """Timestamp donde empezó la racha actual de estado_motor != 0, o None si está detenido."""
    inicio = None
    for ts, valor in reversed(serie):
        if valor == DETENIDO:
            break
        inicio = ts
    return inicio


class ConectorCortina(Conector):
    def al_recibir_datos(self, msg, ctx):
        # Historial previo antes de persistir la muestra actual
        previas = ctx.serie("estado_motor")
        inicio_previo = _inicio_activo(previas)
        ts_previo = previas[-1][0] if previas else None
        estaba_atascado = (
            inicio_previo is not None
            and ts_previo is not None
            and (ts_previo - inicio_previo) >= TIEMPO_MAX_MOTOR
        )

        guardado = ctx.guardar_medicion(msg.values, msg.ts)
        if "estado_motor" not in guardado or msg.ts is None:
            return

        estado = guardado["estado_motor"]
        if estado == DETENIDO:
            if estaba_atascado:
                ctx.alertar(TIPO_ALERTA, "info", "El motor de la cortina volvió a estado detenido")
            return

        inicio = inicio_previo if inicio_previo is not None else msg.ts
        duracion = msg.ts - inicio
        if duracion >= TIEMPO_MAX_MOTOR and not estaba_atascado:
            segundos = int(duracion.total_seconds())
            ctx.alertar(
                TIPO_ALERTA,
                "high",
                f"El motor de la cortina lleva {segundos} s sin detenerse (estado {int(estado)})",
            )
