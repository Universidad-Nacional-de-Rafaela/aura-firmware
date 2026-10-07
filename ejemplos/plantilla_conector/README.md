# Plantilla de conector

Punto de partida para el **conector** de un dispositivo: el código que procesa, dentro de AURA,
lo que manda tu dispositivo. Copiá esta carpeta como `conectores/<CÓDIGO>/` (el mismo código que
la carpeta de tu dispositivo en `dispositivos/`), completá los `TODO` y abrí un pull request.

| Archivo | Qué es |
|---|---|
| `conector.toml` | el manifiesto: nombre, autores, los campos que acepta con su rango, y si predice |
| `conector.py` | la clase, con `al_recibir_datos()` y, si querés, `predecir()` |
| `tests/test_conector.py` | tus tests, con `ContextoDePrueba` (no hace falta AURA ni la placa) |

El ejemplo completo, con una alerta por umbral y una predicción, está en
[`../conector_ejemplo/`](../conector_ejemplo/). La guía, en `CONTRIBUTING.md`, "Tu conector".

Para correrlo contra lo que manda tu placa en el banco:

```bash
herramientas/correr_conector.py conectores/<CÓDIGO> --hw-id mac-<MAC de tu placa> --docker aura-mosquitto
```
