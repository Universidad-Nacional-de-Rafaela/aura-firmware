"""El manifiesto de un conector (conector.toml): quién es, qué campos acepta y con qué rangos.

Es la fuente de los rangos físicos de cada campo. La ficha del dispositivo y los límites del
firmware tienen que coincidir con lo que dice acá.
"""
import re
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

_CAMPO = re.compile(r"^[a-z][a-z0-9_]{0,23}$")
_SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
_REQUISITO = re.compile(r"^(>=|<=|>|<|==)\s*(\d+(?:\.\d+){0,2})$")
_DURACION = re.compile(r"^(\d+)(s|m|h|d)$")
_SEGUNDOS = {"s": 1, "m": 60, "h": 3600, "d": 86400}
_CLASE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*:[A-Za-z_][A-Za-z0-9_]*$")


class ManifiestoInvalido(ValueError):
    """El conector no se puede cargar. El mensaje dice qué corregir."""


@dataclass(frozen=True)
class Campo:
    unidad: str
    min: float
    max: float


@dataclass(frozen=True)
class PrediccionCada:
    cada_s: int
    horizonte_s: int


@dataclass(frozen=True)
class Manifiesto:
    carpeta: Path
    nombre: str
    version: str
    sdk: str
    autores: tuple
    clase: str
    campos: dict
    prediccion: Optional[PrediccionCada] = None


def _version(texto):
    partes = [int(p) for p in texto.split(".")]
    return tuple(partes + [0] * (3 - len(partes)))


def sdk_compatible(requisito: str, version: str) -> bool:
    """">=1,<2" contra "1.0.0". ManifiestoInvalido si el requisito está mal escrito."""
    actual = _version(version)
    partes = [p.strip() for p in requisito.split(",") if p.strip()]
    if not partes:
        raise ManifiestoInvalido(f"sdk: requisito vacío ({requisito!r}); por ejemplo \">=1,<2\"")
    for parte in partes:
        m = _REQUISITO.match(parte)
        if not m:
            raise ManifiestoInvalido(f"sdk: no se entiende {parte!r}; por ejemplo \">=1,<2\"")
        op, v = m.group(1), _version(m.group(2))
        ok = {">=": actual >= v, "<=": actual <= v, ">": actual > v, "<": actual < v, "==": actual == v}[op]
        if not ok:
            return False
    return True


def _duracion(texto, donde):
    m = _DURACION.match(texto) if isinstance(texto, str) else None
    if not m or int(m.group(1)) == 0:
        raise ManifiestoInvalido(f"{donde}: {texto!r} no es una duración; por ejemplo \"15m\", \"6h\", \"1d\"")
    return int(m.group(1)) * _SEGUNDOS[m.group(2)]


def cargar_manifiesto(carpeta) -> Manifiesto:
    from . import __version__

    carpeta = Path(carpeta)
    archivo = carpeta / "conector.toml"
    try:
        datos = tomllib.loads(archivo.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise ManifiestoInvalido(f"falta {archivo}") from None
    except tomllib.TOMLDecodeError as e:
        raise ManifiestoInvalido(f"{archivo}: TOML inválido: {e}") from None

    def texto(clave):
        v = datos.get(clave)
        if not isinstance(v, str) or not v.strip():
            raise ManifiestoInvalido(f"{clave}: falta o no es texto")
        return v.strip()

    nombre = texto("nombre")
    if nombre != carpeta.name:
        raise ManifiestoInvalido(f"nombre: {nombre!r} tiene que ser igual a la carpeta ({carpeta.name!r})")
    version = texto("version")
    if not _SEMVER.match(version):
        raise ManifiestoInvalido(f"version: {version!r} no es X.Y.Z")
    sdk = texto("sdk")
    if not sdk_compatible(sdk, __version__):
        raise ManifiestoInvalido(f"sdk: pide {sdk!r} y este aura_sdk es {__version__}")
    autores = datos.get("autores")
    if not isinstance(autores, list) or not autores or not all(isinstance(a, str) and a for a in autores):
        raise ManifiestoInvalido("autores: lista de usuarios de GitHub, al menos uno")
    clase = texto("clase")
    if not _CLASE.match(clase):
        raise ManifiestoInvalido(f"clase: {clase!r} tiene que ser \"modulo:Clase\", por ejemplo \"conector:ConectorHeladera\"")

    crudos = datos.get("campos")
    if not isinstance(crudos, dict) or not crudos:
        raise ManifiestoInvalido("campos: declarar al menos un campo, con [campos.<nombre>]")
    campos = {}
    for nombre_campo, c in crudos.items():
        if not _CAMPO.match(nombre_campo):
            raise ManifiestoInvalido(f"campos.{nombre_campo}: minúsculas, números y _, hasta 24, "
                                     f"con la unidad en el nombre (temp_c, hum_pct)")
        if not isinstance(c, dict):
            raise ManifiestoInvalido(f"campos.{nombre_campo}: tiene que ser una tabla con unidad, min y max")
        unidad = c.get("unidad")
        if not isinstance(unidad, str) or not unidad:
            raise ManifiestoInvalido(f"campos.{nombre_campo}: falta la unidad")
        mn, mx = c.get("min"), c.get("max")
        if not all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in (mn, mx)):
            raise ManifiestoInvalido(f"campos.{nombre_campo}: min y max tienen que ser números")
        if mn >= mx:
            raise ManifiestoInvalido(f"campos.{nombre_campo}: min ({mn}) tiene que ser menor que max ({mx})")
        campos[nombre_campo] = Campo(unidad=unidad, min=mn, max=mx)

    prediccion = None
    if "prediccion" in datos:
        p = datos["prediccion"]
        if not isinstance(p, dict):
            raise ManifiestoInvalido("prediccion: tiene que ser una tabla con cada y horizonte")
        cada = _duracion(p.get("cada"), "prediccion.cada")
        if cada < 60:
            raise ManifiestoInvalido("prediccion.cada: como mínimo \"1m\"")
        prediccion = PrediccionCada(cada_s=cada, horizonte_s=_duracion(p.get("horizonte"), "prediccion.horizonte"))

    return Manifiesto(carpeta=carpeta, nombre=nombre, version=version, sdk=sdk, autores=tuple(autores),
                      clase=clase, campos=campos, prediccion=prediccion)
