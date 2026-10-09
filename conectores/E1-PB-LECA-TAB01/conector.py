"""Conector del medidor de energía (Schneider PM2130) del tablero del LabECA.

Recibe las mediciones eléctricas de la fase A (tensión, corriente, potencia activa, cos φ y,
si están habilitados, tan φ, THD y energía), las guarda y alerta cuando la instalación sale
de lo normal: tensión fuera de tolerancia, sobrecorriente, potencia negativa (TI invertido) y
bajo cos φ con carga.

Reglas que sigue (contrato de AURA, docs/CONTRATO_MQTT.md):
  - Los rangos físicos los aplica AURA con el manifiesto: lo que está fuera de rango no se
    guarda. Acá no se reemplaza ningún valor que falta.
  - Cada alerta se emite una vez por cruce del umbral, y otra (info) cuando se vuelve a lo
    normal. Para saber cómo estaba antes, se compara con la última medición guardada
    (ctx.serie), no con atributos de la instancia: AURA la puede recrear.
  - Las alertas por umbral las calcula AURA, no el nodo (contrato §3.5).
"""
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Callable, Tuple

from aura_sdk import Conector

# ---- Umbrales. TODO: validarlos con la cátedra y con los datos del tablero ----
V_NOMINAL_V = 220.0
TOLERANCIA_V = 0.10                       # ±10 % de la nominal
V_MIN_V = V_NOMINAL_V * (1 - TOLERANCIA_V)   # 198 V
V_MAX_V = V_NOMINAL_V * (1 + TOLERANCIA_V)   # 242 V

TERMICA_A = 40.0                          # TODO: la protección del circuito del aula
I_ALERTA_A = 0.8 * TERMICA_A              # aviso al 80 % de la protección

P_NEGATIVA_W = -50.0                      # por debajo: energía hacia la red (TI invertido)

COSPHI_MIN = 0.85
P_MIN_COSPHI_W = 200.0                    # con menos carga, el ángulo no es confiable

# Cuánto pasado se mira para saber cómo estaba la medición anterior. Con un envío cada 10 s
# sobra; si no hay nada en la ventana, se toma como "estaba normal".
VENTANA_PREVIA = timedelta(hours=1)


@dataclass(frozen=True)
class Regla:
    tipo: str                                   # va al tópico de la alerta: [a-z_]{1,24}
    severidad: str
    campos: Tuple[str, ...]                     # sin todos estos campos, la regla no se evalúa
    activa: Callable[[dict], bool]
    al_activarse: Callable[[dict], str]
    al_normalizarse: Callable[[dict], str]


REGLAS = (
    Regla("subtension", "warning", ("v_an_v",),
          lambda v: v["v_an_v"] < V_MIN_V,
          lambda v: f"Tensión baja: {v['v_an_v']:.1f} V (mínimo {V_MIN_V:.0f} V)",
          lambda v: f"La tensión volvió a {v['v_an_v']:.1f} V"),
    Regla("sobretension", "warning", ("v_an_v",),
          lambda v: v["v_an_v"] > V_MAX_V,
          lambda v: f"Tensión alta: {v['v_an_v']:.1f} V (máximo {V_MAX_V:.0f} V)",
          lambda v: f"La tensión volvió a {v['v_an_v']:.1f} V"),
    Regla("sobrecorriente", "high", ("i_a_a",),
          lambda v: v["i_a_a"] > I_ALERTA_A,
          lambda v: f"Corriente de {v['i_a_a']:.1f} A: supera el {I_ALERTA_A / TERMICA_A:.0%} "
                    f"de la protección ({TERMICA_A:.0f} A)",
          lambda v: f"La corriente bajó a {v['i_a_a']:.1f} A"),
    Regla("potencia_negativa", "warning", ("p_w",),
          lambda v: v["p_w"] < P_NEGATIVA_W,
          lambda v: f"Potencia activa negativa ({v['p_w']:.0f} W): revisar la polaridad del TI",
          lambda v: f"La potencia activa volvió a ser positiva ({v['p_w']:.0f} W)"),
    Regla("bajo_cosphi", "warning", ("cosphi_a", "p_w"),
          lambda v: v["cosphi_a"] < COSPHI_MIN and v["p_w"] >= P_MIN_COSPHI_W,
          lambda v: f"cos φ de {v['cosphi_a']:.2f} con {v['p_w']:.0f} W de carga "
                    f"(mínimo {COSPHI_MIN:.2f})",
          lambda v: f"Ya no hay bajo cos φ (cos φ {v['cosphi_a']:.2f}, {v['p_w']:.0f} W)"),
)


def _estado(regla, valores):
    """True o False según la regla; None si falta algún campo (no se puede decir nada)."""
    if not all(c in valores for c in regla.campos):
        return None
    return regla.activa(valores)


class ConectorTableroAula(Conector):
    def al_recibir_datos(self, msg, ctx):
        referencia = msg.ts or datetime.now(timezone.utc)

        # La medición anterior de cada campo que usan las reglas, ANTES de guardar esta.
        previo = {}
        for campo in {c for r in REGLAS for c in r.campos}:
            serie = ctx.serie(campo, desde=referencia - VENTANA_PREVIA, hasta=referencia)
            if serie:
                previo[campo] = serie[-1][1]

        guardado = ctx.guardar_medicion(msg.values, msg.ts)   # AURA quita lo fuera de rango
        if not guardado:
            return   # no quedó nada válido: AURA la descarta (sin_campos_validos)

        for regla in REGLAS:
            ahora = _estado(regla, guardado)
            if ahora is None:
                continue                          # falta un campo: no se activa ni se normaliza
            antes = bool(_estado(regla, previo))  # sin dato previo = estaba normal
            if ahora and not antes:
                ctx.alertar(regla.tipo, regla.severidad, regla.al_activarse(guardado))
            elif antes and not ahora:
                ctx.alertar(regla.tipo, "info", regla.al_normalizarse(guardado))
