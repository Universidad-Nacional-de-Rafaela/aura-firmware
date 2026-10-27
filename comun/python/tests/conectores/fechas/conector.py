"""Conector de prueba: guarda con una hora propia."""
from datetime import datetime, timezone

from aura_sdk import Conector

HORA = datetime(2026, 10, 1, 12, 30, 15, 123456, tzinfo=timezone.utc)


class ConectorPrueba(Conector):
    def al_recibir_datos(self, msg, ctx):
        if not isinstance(msg.ts, datetime):
            raise TypeError(f"msg.ts no llegó como datetime: {msg.ts!r}")
        ctx.guardar_medicion(msg.values, ts=HORA)
