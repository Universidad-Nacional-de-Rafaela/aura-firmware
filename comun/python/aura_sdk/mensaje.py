"""El mensaje que recibe un conector: un hw/<hw_id>/data del contrato v4.0 (§3.1)."""
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Optional


@dataclass(frozen=True)
class Mensaje:
    hw_id: str                         # la placa que midió ("mac-…" o "eui-…")
    values: dict                       # las mediciones, tal como las mandó el dispositivo
    ts: Optional[datetime] = None      # hora de la medición, en UTC; None si el nodo no tenía hora
    ingest_id: Optional[str] = None    # identidad de la muestra: igual en cada reintento
    adaptador: Optional[str] = None    # hw_id del raíz o del bridge que la publicó
    extra: dict = field(default_factory=dict)

    @classmethod
    def desde_data(cls, hw_id: str, payload: Any) -> "Mensaje":
        """Arma el mensaje desde el JSON de hw/<hw_id>/data. ValueError si values no es un objeto."""
        if not isinstance(payload, dict) or not isinstance(payload.get("values"), dict):
            raise ValueError("values tiene que ser un objeto JSON")
        conocidos = {"values", "ts", "ingest_id", "adaptador"}
        return cls(hw_id=hw_id, values=dict(payload["values"]), ts=_ts(payload.get("ts")),
                   ingest_id=payload.get("ingest_id") if isinstance(payload.get("ingest_id"), str) else None,
                   adaptador=payload.get("adaptador") if isinstance(payload.get("adaptador"), str) else None,
                   extra={k: v for k, v in payload.items() if k not in conocidos})


def _ts(texto: Any) -> Optional[datetime]:
    # Una hora que no se entiende se ignora: vale la de llegada (contrato §3.1), no se inventa.
    if not isinstance(texto, str):
        return None
    try:
        t = datetime.fromisoformat(texto.replace("Z", "+00:00"))
    except ValueError:
        return None
    return t if t.tzinfo else t.replace(tzinfo=timezone.utc)
