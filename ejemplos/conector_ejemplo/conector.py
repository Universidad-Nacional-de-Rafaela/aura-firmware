"""Conector de ejemplo para ejemplos/sensor_ejemplo (temperatura del chip de la XIAO).

Muestra las tres cosas que hace un conector:
  1. guardar lo que llega (al_recibir_datos);
  2. alertar cuando se cruza un umbral, una vez por cruce y no en cada medición;
  3. predecir (predecir), acá con una recta por mínimos cuadrados sobre la última hora.

No usa atributos de la instancia para recordar nada: AURA puede recrearla en cualquier
momento. Lo que hace falta del pasado se lee de la base con ctx.serie().
"""
from datetime import datetime, timedelta, timezone

from aura_sdk import Conector

UMBRAL_C = 60.0              # temperatura del chip que ya merece un aviso
VENTANA = timedelta(hours=1)  # cuánto pasado mira la predicción
MINIMO_DE_PUNTOS = 4


def recta(puntos):
    """Mínimos cuadrados sobre [(x, y), …]. Devuelve (pendiente, ordenada, desvío de los residuos)."""
    n = len(puntos)
    mx = sum(x for x, _ in puntos) / n
    my = sum(y for _, y in puntos) / n
    sxx = sum((x - mx) ** 2 for x, _ in puntos)
    if sxx == 0:
        return 0.0, my, 0.0
    pendiente = sum((x - mx) * (y - my) for x, y in puntos) / sxx
    ordenada = my - pendiente * mx
    residuos = [y - (pendiente * x + ordenada) for x, y in puntos]
    desvio = (sum(r * r for r in residuos) / max(n - 2, 1)) ** 0.5
    return pendiente, ordenada, desvio


class ConectorEjemplo(Conector):
    def al_recibir_datos(self, msg, ctx):
        temp = msg.values.get("temp_chip_c")
        anterior = ctx.serie("temp_chip_c")[-1:]   # la última guardada, antes de guardar esta

        guardado = ctx.guardar_medicion(msg.values, msg.ts)   # quita lo que esté fuera de rango
        if "temp_chip_c" not in guardado:
            return   # no quedó nada válido: AURA la descarta (sin_campos_validos)

        # Alerta solo en el cruce del umbral, comparando con la medición anterior.
        estaba_alta = bool(anterior) and anterior[0][1] > UMBRAL_C
        if temp > UMBRAL_C and not estaba_alta:
            ctx.alertar("temperatura_alta", "warning", f"El chip está a {temp} °C (umbral {UMBRAL_C} °C)")
        elif temp <= UMBRAL_C and estaba_alta:
            ctx.alertar("temperatura_alta", "info", f"El chip volvió a {temp} °C")

    def predecir(self, ctx):
        ahora = datetime.now(timezone.utc)
        serie = ctx.serie("temp_chip_c", desde=ahora - VENTANA)
        if len(serie) < MINIMO_DE_PUNTOS:
            ctx.log.info("predecir: %d puntos en la última hora, hacen falta %d", len(serie), MINIMO_DE_PUNTOS)
            return
        t0 = serie[0][0]
        puntos = [((ts - t0).total_seconds(), valor) for ts, valor in serie]
        pendiente, ordenada, desvio = recta(puntos)

        objetivo = ahora + timedelta(seconds=self.manifiesto.prediccion.horizonte_s)
        valor = pendiente * (objetivo - t0).total_seconds() + ordenada
        rango = self.manifiesto.campos["temp_chip_c"]
        valor = min(max(valor, rango.min), rango.max)   # una recta no sabe de límites físicos
        ctx.guardar_prediccion("temp_chip_c", objetivo, round(valor, 2),
                               inferior=round(valor - 2 * desvio, 2), superior=round(valor + 2 * desvio, 2),
                               modelo="recta_1h")
