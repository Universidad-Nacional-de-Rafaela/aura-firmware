"""Conector de E2-AZ-TERR-TAN01 — nivel de 3 tanques de agua (terraza del Edificio 2).

Guarda la distancia del sensor al agua de cada tanque y avisa cuando a un tanque le queda menos
del nivel mínimo de agua (alerta `tanque_bajo`), una vez por cruce y no en cada medición. También
avisa cuando un sensor deja de medir o vuelve (alerta `sensor`): el codec no manda el campo de un
sensor que falla, así que acá se nota porque falta.

El dispositivo solo mide: no maneja la bomba. `tanque_bajo` sirve para darse cuenta de que el
control externo de la bomba no llenó a tiempo (bomba, control o provisión de agua).

Lo que no es una medición válida (un valor fuera de 250 a 6000 mm, como los valores especiales
que manda el firmware cuando el sensor falla) lo quita AURA al guardar, según los rangos de
conector.toml. `250` llega cuando el agua está dentro de la zona ciega del sensor: el tanque está
lleno, no es una falla.

No guarda estado en la instancia: AURA la puede recrear. Para saber cómo estaba el tanque
antes, lee las mediciones guardadas con ctx.serie().
"""
from datetime import datetime, timedelta, timezone

from aura_sdk import Conector

# Los campos de conector.toml, en orden: el tanque 1 es el primero.
CAMPOS = ("distancia_tanque1_mm", "distancia_tanque2_mm", "distancia_tanque3_mm")


class ConectorTanques(Conector):
    # ---- VALORES DE PRUEBA: ajustar cuando se midan los tanques ----
    # Distancia del sensor (en la tapa) al fondo de cada tanque, en mm: lo que mediría el
    # sensor con el tanque vacío. 1630 es una estimación sin confirmar; probablemente sean
    # más grandes. Si los tres tanques son iguales, va el mismo número tres veces.
    ALTURA_MM = (1630, 1630, 1630)
    # Fracción del tanque con agua por debajo de la cual se avisa `tanque_bajo`.
    NIVEL_MINIMO = 0.20
    # Margen para no avisar varias veces si el nivel queda justo en el umbral: el aviso de que
    # el tanque se recuperó sale recién cuando el agua sube este margen por encima del umbral.
    HISTERESIS_MM = 50
    # Hasta dónde mira hacia atrás para saber cómo estaba el tanque.
    VENTANA = timedelta(hours=24)

    def umbral_mm(self, indice):
        """Distancia del sensor al agua (mm) a partir de la cual el tanque está bajo.

        Con 1630 mm de altura y 20 % de mínimo: 1630 - 326 = 1304 mm.
        """
        return round(self.ALTURA_MM[indice] * (1 - self.NIVEL_MINIMO))

    def al_recibir_datos(self, msg, ctx):
        ts = msg.ts or datetime.now(timezone.utc)
        # Las mediciones guardadas de cada tanque, ANTES de guardar las de este mensaje.
        anteriores = {campo: ctx.serie(campo, desde=ts - self.VENTANA) for campo in CAMPOS}

        guardado = ctx.guardar_medicion(msg.values, msg.ts)   # quita lo inválido
        if not guardado:
            return   # no quedó nada válido: AURA descarta el mensaje (sin_campos_validos)

        self._avisar_sensores(guardado, anteriores, ctx)

        for indice, campo in enumerate(CAMPOS):
            if campo not in guardado:
                continue
            numero = indice + 1
            distancia = guardado[campo]
            umbral = self.umbral_mm(indice)
            estaba_bajo = self._estaba_bajo(anteriores[campo], umbral)
            if estaba_bajo:
                esta_bajo = distancia > umbral - self.HISTERESIS_MM
            else:
                esta_bajo = distancia > umbral
            # Alerta solo en el cruce, no en cada medición.
            if esta_bajo and not estaba_bajo:
                ctx.alertar("tanque_bajo", "warning",
                            f"Tanque {numero}: queda menos del {self.NIVEL_MINIMO:.0%} de agua "
                            f"(el agua está a {distancia} mm del sensor, umbral {umbral} mm)")
            elif not esta_bajo and estaba_bajo:
                ctx.alertar("tanque_bajo", "info",
                            f"Tanque {numero}: el agua volvió a {distancia} mm del sensor "
                            f"(más del {self.NIVEL_MINIMO:.0%})")

    def _estaba_bajo(self, puntos, umbral):
        """Si el tanque estaba bajo, según las mediciones anteriores y sin guardar estado.

        Busca hacia atrás la última medición fuera de la banda de histéresis: por encima del
        umbral estaba bajo; por debajo de umbral - histéresis, no. Si todas caen en la banda
        (o no hay), se toma como que no estaba bajo.
        """
        for _, valor in reversed(puntos):
            if valor > umbral:
                return True
            if valor <= umbral - self.HISTERESIS_MM:
                return False
        return False

    @staticmethod
    def _avisar_sensores(guardado, anteriores, ctx):
        """Alerta `sensor` una vez por falla y una vez por recuperación, sin guardar estado.

        Un tanque "ya fallaba" si su última medición es anterior a la del último mensaje que
        trajo algo (el máximo entre los tres tanques). Si fallan los tres juntos, AURA descarta
        el mensaje antes de llegar acá y no hay alerta: suele ser el nodo, no los sensores.
        """
        ultimos = {campo: anteriores[campo][-1][0] for campo in CAMPOS if anteriores[campo]}
        ultimo_mensaje = max(ultimos.values(), default=None)
        for numero, campo in enumerate(CAMPOS, start=1):
            falla = campo not in guardado
            ultimo_del_tanque = ultimos.get(campo)
            fallaba = ultimo_mensaje is not None and (
                ultimo_del_tanque is None or ultimo_del_tanque < ultimo_mensaje)
            if falla and not fallaba:
                ctx.alertar("sensor", "warning",
                            f"Tanque {numero}: el sensor no dio una medición válida")
            elif not falla and fallaba:
                ctx.alertar("sensor", "info", f"Tanque {numero}: el sensor volvió a medir")
