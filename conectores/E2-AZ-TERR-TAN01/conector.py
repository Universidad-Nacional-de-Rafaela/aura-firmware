"""Conector de E2-AZ-TERR-TAN01 — nivel de 3 tanques de agua (terraza del Edificio 2).

Guarda la distancia del sensor al agua de cada tanque y avisa cuando un tanque queda bajo
(el agua se aleja del sensor), una vez por cruce del umbral y no en cada medición.

Lo que no es una medición válida (un valor fuera de 250 a 6000 mm, como los valores
especiales que manda el firmware cuando el sensor falla) lo quita AURA al guardar, según
los rangos de conector.toml: acá no hace falta revisarlo.

No guarda estado en la instancia: AURA la puede recrear. Para saber cómo estaba el tanque
antes, lee la última medición guardada con ctx.serie().
"""
from aura_sdk import Conector

# Los campos de conector.toml, en orden: el tanque 1 es el primero.
CAMPOS = ("distancia_tanque1_mm", "distancia_tanque2_mm", "distancia_tanque3_mm")


class ConectorTanques(Conector):
    # TODO: ajustar con el tanque real. Distancia del sensor al agua, en mm, a partir de la
    # cual el tanque se considera bajo: altura del sensor sobre el fondo, menos el nivel
    # mínimo que se quiere tener. Con el valor provisional de 4000 mm el aviso funciona, pero
    # no significa nada hasta medir los tanques.
    UMBRAL_BAJO_MM = 4000

    def al_recibir_datos(self, msg, ctx):
        # La última medición guardada de cada tanque, ANTES de guardar la de este mensaje.
        anteriores = {campo: ctx.serie(campo)[-1:] for campo in CAMPOS if campo in msg.values}

        guardado = ctx.guardar_medicion(msg.values, msg.ts)   # quita lo inválido
        if not guardado:
            return   # no quedó nada válido: AURA descarta el mensaje (sin_campos_validos)

        for numero, campo in enumerate(CAMPOS, start=1):
            if campo not in guardado:
                continue
            distancia = guardado[campo]
            anterior = anteriores[campo]
            estaba_bajo = bool(anterior) and anterior[0][1] > self.UMBRAL_BAJO_MM
            esta_bajo = distancia > self.UMBRAL_BAJO_MM
            # Alerta solo en el cruce del umbral, comparando con la medición anterior.
            if esta_bajo and not estaba_bajo:
                ctx.alertar("tanque_bajo", "warning",
                            f"Tanque {numero}: el agua está a {distancia} mm del sensor "
                            f"(umbral {self.UMBRAL_BAJO_MM} mm)")
            elif not esta_bajo and estaba_bajo:
                ctx.alertar("tanque_bajo", "info",
                            f"Tanque {numero}: el agua volvió a {distancia} mm del sensor")
