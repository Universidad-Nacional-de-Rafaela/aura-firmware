"""Conector de E1-PB-LECA-LUZ01 — control de iluminación del aula del LabECA.

Guarda la presencia que manda la placa (LD2420) y alerta una sola vez por cruce de estado
("aula ocupada" / "aula libre"), no en cada medición. Los demás campos del manifiesto
(lux de los BH1750, temperatura y humedad del SHT30 y estado del relé) se guardan tal cual
llegan, sin alerta: el firmware los manda solo cuando ese hardware responde, y si un sensor
falla directamente no aparece en la muestra.

La decisión de prender o apagar la luz NO pasa por acá: la toma la lógica local del firmware
(luz apagada sin gente; con gente, BAJO/ALTO según los umbrales); AURA solo recibe la serie
y las alertas. El conector no actúa.

Reglas (contrato de AURA, docs/CONTRATO_MQTT.md):
  - Solo se guardan mediciones válidas: lo que no esté en el manifiesto o esté fuera de rango
    lo quita AURA. No reemplaces un valor que falta por otro ni por un valor fijo.
  - Si una muestra no sirve y no hay que pedir que se reenvíe, ctx.descartar("motivo").
  - Si lanzás una excepción, AURA guarda la muestra cruda (solo los campos válidos), agrega la
    alerta `conector_fallo` y confirma con `motivo = "conector_fallo"`; el dispositivo no
    reenvía.
  - No guardes estado en atributos de la instancia: AURA la puede recrear. Para mirar el
    pasado, ctx.serie(campo, desde, hasta).
"""
from aura_sdk import Conector
from datetime import datetime, timedelta, timezone


class ConectorLuz01(Conector):
    def al_recibir_datos(self, msg, ctx):
        # msg.values: las mediciones ({"presencia": 1}); msg.ts: la hora de la medición (o None);
        # msg.hw_id: la placa.
        referencia = msg.ts if getattr(msg, "ts", None) is not None else datetime.now(timezone.utc)
        desde = referencia - timedelta(days=1)
        anterior = ctx.serie("presencia", desde=desde, hasta=referencia)[-1:]   # último estado anterior a esta muestra

        guardado = ctx.guardar_medicion(msg.values, msg.ts)   # quita lo que esté fuera de rango
        if "presencia" not in guardado:
            return   # sin presencia válida no hay cruce que alertar (otros campos pueden haberse guardado)

        presencia = guardado["presencia"]
        estaba = bool(anterior) and anterior[0][1] > 0
        if presencia > 0 and not estaba:
            ctx.alertar("presencia", "info", "Aula ocupada")
        elif presencia == 0 and estaba:
            ctx.alertar("presencia", "info", "Aula libre")
