# conector_ejemplo

Conector completo para [`sensor_ejemplo`](../sensor_ejemplo/) (temperatura del chip de la XIAO,
campo `temp_chip_c`). Muestra lo que hace un conector:

| Qué | Dónde | Cómo |
|---|---|---|
| Guardar | `al_recibir_datos()` | `ctx.guardar_medicion(msg.values, msg.ts)`; lo que está fuera del rango del manifiesto lo quita AURA |
| Alertar | `al_recibir_datos()` | `temperatura_alta` al pasar los 60 °C y al volver, **una vez por cruce**: compara con la última medición guardada (`ctx.serie`), no con un atributo de la instancia |
| Predecir | `predecir()`, cada 15 min | recta por mínimos cuadrados sobre la última hora, a una hora vista, con una banda de ±2 desvíos |

Probarlo:

```bash
python3 -m unittest discover -s ejemplos/conector_ejemplo/tests
herramientas/correr_conector.py ejemplos/conector_ejemplo --hw-id mac-<MAC de la placa> --docker aura-mosquitto
```

Para el conector de tu dispositivo, copiá [`../plantilla_conector/`](../plantilla_conector/).
