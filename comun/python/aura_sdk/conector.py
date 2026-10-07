"""La clase de la que hereda el conector de cada grupo."""
from .manifiesto import Manifiesto


class Conector:
    """Procesa los mensajes de un dispositivo.

    AURA crea una instancia por dispositivo asignado y la reutiliza, pero puede recrearla en
    cualquier momento (un reinicio, una versión nueva): lo que tenga que sobrevivir va a la
    base, con el Contexto, y no en atributos de la instancia.
    """

    def __init__(self, manifiesto: Manifiesto):
        self.manifiesto = manifiesto

    def al_recibir_datos(self, msg, ctx) -> None:
        """Obligatorio. Recibe un Mensaje y decide: ctx.guardar_medicion(...) o ctx.descartar(...).

        Si no llama a ninguno, la muestra se descarta ("sin_accion"). Si lanza una excepción,
        no se guarda nada y AURA no confirma: el dispositivo la va a reenviar.
        """
        raise NotImplementedError

    def predecir(self, ctx) -> None:
        """Opcional. Se llama cada [prediccion].cada del manifiesto. Lee con ctx.serie(...) y
        guarda con ctx.guardar_prediccion(...)."""
