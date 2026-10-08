"""El Contexto: lo único que un conector puede tocar.

Contexto es la interfaz. En AURA la implementa el runtime (que habla con la base); acá está
ContextoDePrueba, que guarda todo en memoria y se comporta igual, para probar un conector sin
AURA. La suite de aura_sdk.contrato verifica que las dos implementaciones no diverjan.
"""
import logging
import re
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Optional

SEVERIDADES = ("info", "warning", "high", "critical")
_TIPO_ALERTA = re.compile(r"^[a-z_]{1,24}$")


@dataclass(frozen=True)
class Medicion:
    ts: datetime
    values: dict
    ingest_id: Optional[str] = None


@dataclass(frozen=True)
class Prediccion:
    campo: str
    ts_objetivo: datetime
    valor: float
    inferior: Optional[float]
    superior: Optional[float]
    modelo: str
    generado_en: datetime


@dataclass(frozen=True)
class Alerta:
    tipo: str
    severidad: str
    mensaje: str


def _es_numero(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and v == v   # v == v descarta NaN


def _utc(ts):
    if ts is None:
        return datetime.now(timezone.utc)
    if not isinstance(ts, datetime):
        raise TypeError("ts tiene que ser un datetime")
    return ts if ts.tzinfo else ts.replace(tzinfo=timezone.utc)


class Contexto:
    """Interfaz. Todo lo que guarda queda atado al dispositivo del conector: no se elige."""

    log: logging.Logger

    def guardar_medicion(self, values: dict, ts: Optional[datetime] = None) -> dict:
        """Guarda las mediciones. Quita los campos no declarados en el manifiesto o fuera de
        rango, y devuelve lo que guardó. Si no queda ninguno, la muestra se descarta."""
        raise NotImplementedError

    def descartar(self, motivo: str) -> None:
        """La muestra no se guarda y no hay que pedir que se reenvíe."""
        raise NotImplementedError

    def serie(self, campo: str, desde: Optional[datetime] = None, hasta: Optional[datetime] = None) -> list:
        """Historial de un campo de este dispositivo: [(ts, valor), …], de la más vieja a la más nueva."""
        raise NotImplementedError

    def guardar_prediccion(self, campo: str, ts_objetivo: datetime, valor: float,
                           inferior: Optional[float] = None, superior: Optional[float] = None,
                           modelo: str = "") -> None:
        raise NotImplementedError

    def alertar(self, tipo: str, severidad: str, mensaje: str) -> None:
        """Alerta del dispositivo (por ejemplo, un umbral cruzado). La publica AURA."""
        raise NotImplementedError

    def config_vigente(self) -> dict:
        """La última configuración que reportó el dispositivo (set_config, contrato §3.2)."""
        raise NotImplementedError


class ContextoDePrueba(Contexto):
    """Contexto en memoria, con las mismas reglas que el de AURA."""

    def __init__(self, manifiesto, config: Optional[dict] = None):
        self.manifiesto = manifiesto
        self._config = dict(config or {})
        self.mediciones: list = []
        self.predicciones: list = []
        self.alertas: list = []
        self.descartes: list = []
        self.log = logging.getLogger(f"conector.{manifiesto.nombre}")
        self._vistos = set()
        self._msg = None
        self._pendiente = None   # lo que hizo el conector con el mensaje en curso

    # ----- lo que usa el conector -----

    def guardar_medicion(self, values, ts=None):
        if not isinstance(values, dict):
            raise TypeError("values tiene que ser un dict")
        validos, quitados = {}, []
        for campo, valor in values.items():
            c = self.manifiesto.campos.get(campo)
            if c is None or not _es_numero(valor) or not (c.min <= valor <= c.max):
                quitados.append(campo)
                continue
            validos[campo] = valor
        if quitados:
            self.log.warning("campos quitados (no declarados, no numéricos o fuera de rango): %s", quitados)
        if not validos:
            self._registrar(("descartado", "sin_campos_validos", None, quitados))
            return {}
        med = Medicion(ts=_utc(ts), values=validos, ingest_id=self._msg.ingest_id if self._msg else None)
        self._registrar(("persistido", None, med, quitados))
        return dict(validos)

    def descartar(self, motivo):
        if not isinstance(motivo, str) or not motivo:
            raise ValueError("descartar necesita un motivo")
        self._registrar(("descartado", motivo, None, []))

    def serie(self, campo, desde=None, hasta=None):
        if campo not in self.manifiesto.campos:
            raise KeyError(f"{campo} no está declarado en el manifiesto")
        d = _utc(desde) if desde else None
        h = _utc(hasta) if hasta else None
        puntos = [(m.ts, m.values[campo]) for m in self.mediciones if campo in m.values
                  and (d is None or m.ts >= d) and (h is None or m.ts <= h)]
        return sorted(puntos, key=lambda p: p[0])

    def guardar_prediccion(self, campo, ts_objetivo, valor, inferior=None, superior=None, modelo=""):
        if campo not in self.manifiesto.campos:
            raise KeyError(f"{campo} no está declarado en el manifiesto")
        if not _es_numero(valor):
            raise ValueError("valor tiene que ser un número")
        for limite in (inferior, superior):
            if limite is not None and not _es_numero(limite):
                raise ValueError("inferior y superior tienen que ser números o None")
        if inferior is not None and inferior > valor or superior is not None and superior < valor:
            raise ValueError("tiene que cumplirse inferior <= valor <= superior")
        self.predicciones.append(Prediccion(campo=campo, ts_objetivo=_utc(ts_objetivo), valor=valor,
                                            inferior=inferior, superior=superior, modelo=str(modelo),
                                            generado_en=datetime.now(timezone.utc)))

    def alertar(self, tipo, severidad, mensaje):
        if not isinstance(tipo, str) or not _TIPO_ALERTA.match(tipo):
            raise ValueError("tipo: minúsculas y _, hasta 24 caracteres")
        if severidad not in SEVERIDADES:
            raise ValueError(f"severidad: una de {SEVERIDADES}")
        self.alertas.append(Alerta(tipo=tipo, severidad=severidad, mensaje=str(mensaje)))

    def config_vigente(self):
        return dict(self._config)

    # ----- lo que usa procesar() (en AURA, el runtime) -----

    def _ya_visto(self, ingest_id):
        return ingest_id is not None and ingest_id in self._vistos

    def _empezar(self, msg):
        self._msg = msg
        self._pendiente = None

    def _registrar(self, accion):
        # La última decisión del conector sobre el mensaje es la que vale.
        if self._msg is None:
            raise RuntimeError("guardar_medicion y descartar solo se llaman dentro de al_recibir_datos")
        self._pendiente = accion

    def _confirmar(self):
        """Commit: devuelve (resultado, motivo, quitados)."""
        accion, self._pendiente, msg, self._msg = self._pendiente, None, self._msg, None
        if accion is None:
            accion = ("descartado", "sin_accion", None, [])
        resultado, motivo, medicion, quitados = accion
        if medicion is not None:
            self.mediciones.append(medicion)
        if resultado == "descartado":
            self.descartes.append((msg, motivo))
        if msg is not None and msg.ingest_id:
            self._vistos.add(msg.ingest_id)
        return resultado, motivo, quitados

    def _deshacer(self):
        """Rollback: lo que hizo el conector con este mensaje no queda."""
        self._pendiente = None
        self._msg = None
