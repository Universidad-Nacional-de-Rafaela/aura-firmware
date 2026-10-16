"""Conector de E1-AZ-TERR-TAN01 — nivel de dos tanques de agua de la terraza (LoRaWAN).

El nodo manda solo la distancia del sensor al agua de cada tanque (dist_tN_cm). Con las medidas
de TANQUES, el conector calcula el nivel y el volumen, y guarda los tres. Para otro tanque, o si
una medida estaba mal, se cambia TANQUES: el firmware no se toca y la distancia guardada permite
recalcular la historia.

Avisa cuando un tanque baja del UMBRAL_PCT (nivel_bajo) o llega al UMBRAL_ALTO_PCT (nivel_alto),
y cuando vuelve: una vez por cruce, no en cada medición. Si la distancia es menor que la de lleno,
el agua está por encima de la marca o el sensor está obstruido (condensación): desde acá no se
distingue, y el aviso lo dice.

No usa atributos de la instancia para recordar nada: AURA puede recrearla en cualquier
momento. Para saber si el tanque ya estaba bajo, mira la última medición guardada con ctx.serie().
"""
from aura_sdk import Conector

# Medidas de cada tanque: distancia del sensor al agua con el tanque vacío y lleno, y capacidad.
# El volumen supone sección constante (tanque cilíndrico o de paredes rectas).
TANQUES = {
    1: {"vacio_cm": 170, "lleno_cm": 20, "capacidad_l": 1000},
    2: {"vacio_cm": 200, "lleno_cm": 20, "capacidad_l": 1500},
}

UMBRAL_PCT = 20        # por debajo de este nivel, el tanque merece un aviso
UMBRAL_ALTO_PCT = 95   # desde este nivel, el tanque está lleno


def nivel_y_volumen(dist_cm, tanque):
    """Nivel en % y volumen en L a partir de la distancia. Fuera del rango vacío-lleno (el ruido
    del sensor, o el agua por encima de la marca de lleno) se acota a 0 o 100 %."""
    fraccion = (tanque["vacio_cm"] - dist_cm) / (tanque["vacio_cm"] - tanque["lleno_cm"])
    fraccion = min(max(fraccion, 0.0), 1.0)
    return round(fraccion * 100, 1), round(fraccion * tanque["capacidad_l"])


class ConectorTanques(Conector):
    def al_recibir_datos(self, msg, ctx):
        # La última guardada de cada tanque, antes de guardar esta.
        anteriores = {n: ctx.serie(f"nivel_t{n}_pct")[-1:] for n in TANQUES}

        values = {}
        for n, tanque in TANQUES.items():
            campo = f"dist_t{n}_cm"
            if campo not in msg.values:
                continue   # ese sensor no midió: no se calcula nada
            dist = msg.values[campo]
            values[campo] = dist
            rango = self.manifiesto.campos[campo]
            if isinstance(dist, (int, float)) and not isinstance(dist, bool) and rango.min <= dist <= rango.max:
                values[f"nivel_t{n}_pct"], values[f"vol_t{n}_l"] = nivel_y_volumen(dist, tanque)

        guardado = ctx.guardar_medicion(values, msg.ts)   # quita lo que esté fuera de rango
        for n, tanque in TANQUES.items():
            campo = f"nivel_t{n}_pct"
            if campo not in guardado:
                continue   # ese tanque no se midió, o vino fuera de rango: nada que comparar
            nivel = guardado[campo]
            anterior = anteriores[n][0][1] if anteriores[n] else None

            estaba_bajo = anterior is not None and anterior < UMBRAL_PCT
            if nivel < UMBRAL_PCT and not estaba_bajo:
                ctx.alertar("nivel_bajo", "warning", f"Tanque {n} al {nivel} % (umbral {UMBRAL_PCT} %)")
            elif nivel >= UMBRAL_PCT and estaba_bajo:
                ctx.alertar("nivel_bajo", "info", f"Tanque {n} volvió al {nivel} %")

            estaba_alto = anterior is not None and anterior >= UMBRAL_ALTO_PCT
            if nivel >= UMBRAL_ALTO_PCT and not estaba_alto:
                dist = guardado[f"dist_t{n}_cm"]
                if dist < tanque["lleno_cm"]:
                    texto = f"Tanque {n} lleno o rebalsando, o el sensor está obstruido ({dist} cm del sensor)"
                else:
                    texto = f"Tanque {n} lleno, al {nivel} % (umbral {UMBRAL_ALTO_PCT} %)"
                ctx.alertar("nivel_alto", "warning", texto)
            elif nivel < UMBRAL_ALTO_PCT and estaba_alto:
                ctx.alertar("nivel_alto", "info", f"Tanque {n} bajó al {nivel} %")
