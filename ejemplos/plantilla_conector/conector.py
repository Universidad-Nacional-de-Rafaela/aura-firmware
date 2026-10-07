"""Conector de <CÓDIGO> — <qué dispositivo es>.

TODO: describir en dos líneas qué hace este conector con los datos.

Reglas (contrato de AURA, docs/CONTRATO_MQTT.md):
  - Solo se guardan mediciones válidas: lo que no esté en el manifiesto o esté fuera de rango
    lo quita AURA. No reemplaces un valor que falta por otro ni por un valor fijo.
  - Si una muestra no sirve y no hay que pedir que se reenvíe, ctx.descartar("motivo").
  - Si lanzás una excepción, no se guarda nada y el dispositivo la reenvía más tarde: úsenlo
    solo para errores de verdad, no para "este dato no me gusta".
  - No guardes estado en atributos de la instancia: AURA la puede recrear. Para mirar el
    pasado, ctx.serie(campo, desde, hasta).
"""
from aura_sdk import Conector


class ConectorPlantilla(Conector):
    def al_recibir_datos(self, msg, ctx):
        # msg.values: las mediciones ({"temp_c": 4.5}); msg.ts: la hora de la medición (o None);
        # msg.hw_id: la placa. TODO: validaciones propias de tu dispositivo, si hacen falta.
        ctx.guardar_medicion(msg.values, msg.ts)

        # TODO (opcional): alertas propias, una vez por cruce y no en cada medición. Ver
        # ejemplos/conector_ejemplo/conector.py.

    # TODO (opcional): predecir. Ver ejemplos/conector_ejemplo/conector.py.
    # def predecir(self, ctx):
    #     serie = ctx.serie("temp_c", desde=...)
    #     ctx.guardar_prediccion("temp_c", ts_objetivo, valor, inferior, superior, modelo="...")
