"""Conector AURA v4: mediciones de heladera/freezer y alertas por cruce.

No usa red, archivos ni estado de instancia. Las decisiones se apoyan en
config_vigente() y serie(): sobreviven a una recreacion del conector.
"""
from datetime import timezone
from math import isfinite

from aura_sdk import Conector

CAMPOS = {
    "temp_heladera_c": ("heladera", "hlo", "hhi", 2.0, 6.0),
    "temp_freezer_c": ("freezer", "flo", "fhi", -25.0, -15.0),
}


def numero(valor):
    return (isinstance(valor, int) and not isinstance(valor, bool)) or (isinstance(valor, float) and isfinite(valor))


def evaluar(estado, valor, inferior, superior, margen):
    activa, pendiente = estado
    zona = -1 if valor < inferior else 1 if valor > superior else 0
    if zona != 0 and zona == pendiente:
        activa = zona
    if zona == 0 and (
        (activa < 0 and valor >= inferior + margen)
        or (activa > 0 and valor <= superior - margen)
    ):
        activa = 0
    return activa, zona


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
        anteriores = {campo: ctx.serie(campo) for campo in validos}
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
            if anterior and instante is not None and instante < hora_utc(anterior[-1][0]):
                ctx.log.info("%s: lectura historica guardada sin cambiar alarma actual", campo)
                continue
            # Trabajar en centésimas evita diferencias en los límites con el firmware.
            minimo, maximo = round(inferior * 100), round(superior * 100)
            margen = min(50, (maximo - minimo) / 2)
            estado = (0, 0)
            for _, temperatura in anterior:
                # D5 puede guardar el centinela en crudo si el conector falló.
                if numero(temperatura) and -55 <= temperatura <= 125 and temperatura != 85:
                    estado = evaluar(estado, round(temperatura * 100), minimo, maximo, margen)
            actual, _ = evaluar(estado, round(valor * 100), minimo, maximo, margen)
            if actual == estado[0]:
                continue

            fecha = f" (medición {instante.isoformat()})" if instante is not None else " (sin hora de medición)"
            if actual == 0:
                ctx.alertar(f"temperatura_{nombre}", "info",
                            f"{nombre.capitalize()} recuperada: {valor} °C{fecha}")
            else:
                ctx.alertar(f"temperatura_{nombre}", "warning",
                            f"{nombre.capitalize()}: {valor} °C, fuera de {inferior}..{superior} °C{fecha}")
