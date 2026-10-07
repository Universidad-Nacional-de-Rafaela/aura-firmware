"""Carga un conector desde su carpeta: manifiesto + clase."""
import importlib.util
import sys
from pathlib import Path

from .conector import Conector
from .manifiesto import ManifiestoInvalido, cargar_manifiesto


def cargar_conector(carpeta):
    """Devuelve (manifiesto, instancia). ManifiestoInvalido si algo no está bien."""
    m = cargar_manifiesto(carpeta)
    modulo, nombre_clase = m.clase.split(":")
    archivo = Path(carpeta) / f"{modulo}.py"
    if not archivo.is_file():
        raise ManifiestoInvalido(f"clase: no existe {archivo}")
    # Nombre propio por carpeta: dos conectores con un conector.py cada uno no se pisan.
    nombre_modulo = f"aura_conector_{Path(carpeta).resolve().name.replace('-', '_')}_{modulo}"
    spec = importlib.util.spec_from_file_location(nombre_modulo, archivo)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[nombre_modulo] = mod
    try:
        spec.loader.exec_module(mod)
    except Exception as e:
        del sys.modules[nombre_modulo]
        raise ManifiestoInvalido(f"{archivo}: no se pudo importar: {e!r}") from e
    clase = getattr(mod, nombre_clase, None)
    if not isinstance(clase, type) or not issubclass(clase, Conector):
        raise ManifiestoInvalido(f"clase: {nombre_clase} tiene que existir en {archivo.name} y heredar de aura_sdk.Conector")
    if clase.al_recibir_datos is Conector.al_recibir_datos:
        raise ManifiestoInvalido(f"clase: {nombre_clase} tiene que implementar al_recibir_datos(self, msg, ctx)")
    return m, clase(m)
