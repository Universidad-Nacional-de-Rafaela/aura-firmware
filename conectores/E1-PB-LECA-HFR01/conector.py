"""Conector AURA v4: mediciones de heladera/freezer y alertas por cruce.

No usa red, archivos ni estado de instancia. Las decisiones se apoyan en
config_vigente() y serie(): sobreviven a una recreacion del conector.
"""
from datetime import datetime, timezone
from math import isfinite

from aura_sdk import Conector

CAMPOS = {
    "temp_heladera_c": ("heladera", "hlo", "hhi", 2.0, 6.0),
    "temp_freezer_c": ("freezer", "flo", "fhi", -25.0, -15.0),
}


def numero(valor):
    return (isinstance(valor, int) and not isinstance(valor, bool)) or (isinstance(valor, float) and isfinite(valor))


def zona(valor, inferior, superior):
    return -1 if valor < inferior else 1 if valor > superior else 0


def hora_utc(ts):
    if ts is None:
        return None
    return ts.replace(tzinfo=timezone.utc) if ts.tzinfo is None else ts.astimezone(timezone.utc)


class ConectorHeladeraFreezer(Conector):
    def al_recibir_datos(self, msg, ctx):
        validos = {}
        for campo in CAMPOS:
            valor = msg.values.get(campo)
            limites = self.manifiesto.campos[campo]
            if numero(valor) and limites.min <= valor <= limites.max and valor != 85:
                validos[campo] = valor
            elif campo in msg.values:
                ctx.log.warning("%s: valor fisicamente invalido; se omite", campo)

        if not validos:
            ctx.descartar("sin_temperaturas_validas")
            return

        # Leer antes de guardar: el contexto aplica su commit al terminar.
        anteriores = {campo: ctx.serie(campo)[-1:] for campo in validos}
        guardados = ctx.guardar_medicion(validos, msg.ts)
        config = ctx.config_vigente()
        instante = hora_utc(msg.ts)

        for campo, valor in guardados.items():
            nombre, kmin, kmax, default_min, default_max = CAMPOS[campo]
            inferior, superior = config.get(kmin, default_min), config.get(kmax, default_max)
            if not (numero(inferior) and numero(superior) and -55 <= inferior < superior <= 125):
                ctx.log.warning("%s: config de umbrales invalida; se usan valores iniciales", campo)
                inferior, superior = default_min, default_max

            anterior = anteriores[campo]
            # Una cola puede entregar datos historicos fuera de orden. Se guardan,
            # pero no cambian el estado actual de la alarma ni disparan recuperaciones.
            if anterior and instante is not None and instante < hora_utc(anterior[0][0]):
                ctx.log.info("%s: lectura historica guardada sin cambiar alarma actual", campo)
                continue
            previa = zona(anterior[0][1], inferior, superior) if anterior else 0
            actual = zona(valor, inferior, superior)
            if actual == previa:
                continue
            if actual == 0:
                ctx.alertar(f"temperatura_{nombre}", "info", f"{nombre.capitalize()} recuperada: {valor} °C")
            else:
                ctx.alertar(f"temperatura_{nombre}", "warning",
                            f"{nombre.capitalize()}: {valor} °C, fuera de {inferior}..{superior} °C")
