"""aura_sdk: lo que necesita un grupo para escribir el conector de su dispositivo.

Un conector es el código que procesa, dentro de AURA, los datos que manda un dispositivo:
los valida, los guarda y, si quiere, predice. Corre aislado: lo único que puede tocar es el
Contexto que le pasa AURA. Ver ejemplos/conector_ejemplo/ y CONTRIBUTING.md.

Python 3.11 o posterior, sin dependencias.
"""
from .conector import Conector
from .contexto import Alerta, Contexto, ContextoDePrueba, Medicion, Prediccion
from .ejecutar import ResultadoMensaje, procesar
from .manifiesto import Campo, Manifiesto, ManifiestoInvalido, PrediccionCada, cargar_manifiesto
from .cargar import cargar_conector
from .mensaje import Mensaje

__version__ = "1.0.0"

__all__ = [
    "Alerta", "Campo", "Conector", "Contexto", "ContextoDePrueba", "Manifiesto",
    "ManifiestoInvalido", "Medicion", "Mensaje", "Prediccion", "PrediccionCada",
    "ResultadoMensaje", "cargar_conector", "cargar_manifiesto", "procesar", "__version__",
]
