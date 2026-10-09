# Conector de heladera y freezer IC IV

Procesa las mediciones de una placa con hw_id y dos campos independientes:
temp_heladera_c / temp_freezer_c. Python 3.11 o posterior; usa exclusivamente
el SDK aura_sdk de comun/python/, sin paquetes externos ni acceso a red/archivos.

| Archivo | Función |
|---|---|
| conector.toml | Código, usuarios del grupo, unidad y rango físico de las dos sondas. |
| conector.py | Validación, guardado y alertas por cruce de umbrales. |
| tests/test_conector.py | 20 pruebas con ContextoDePrueba, sin AURA ni hardware. |

## Decisiones

Solo guarda temperaturas numéricas finitas entre -55 y 125 °C. Omite 85 °C,
-127, null, booleanos, cadenas, campos desconocidos y diagnóstico. Si una sonda
es válida y la otra falla, guarda solo la válida. Si no queda ninguna, descarta
con motivo sin_temperaturas_validas. Conserva msg.ts; si falta, el SDK asigna
hora de llegada. No inventa una hora de medición.

El runtime deduplica ingest_id antes de invocar el conector. Una excepción
produce ausencia de ACK y deja la muestra para reintento. El conector no publica
ACK por su cuenta: decide guardar/descartar con el Contexto.

Alertas independientes: temperatura_heladera / temperatura_freezer, warning al
salir del rango de conservación e info al recuperarse. Una por cambio, no una
por cada lectura. Usa hlo/hhi y flo/fhi de config_vigente(), con valores iniciales
2..6 y -25..-15 °C. Una configuración inválida usa los valores iniciales y se
registra. El pasado sale de ctx.serie(): recrear la instancia no reinicia la alarma.

Las lecturas anteriores al último punto guardado se conservan como historial,
sin alterar la alarma actual. Si llegan sin ts se usa su orden de llegada;
no se puede distinguir un dato histórico sin timestamp. Cambiar umbrales vuelve
a comparar con la configuración vigente; el SDK no ofrece historial de ajustes.

Este conector no implementa predicción y no la declara en el manifiesto. Las fallas físicas de sondas/energía son alertas del firmware, no
temperaturas inventadas por el conector.

## Probar

Desde la raíz del repo:

```powershell
python -m unittest discover -s conectores/E1-PB-LECA-HFR01/tests -v
python herramientas/validar_conectores.py
```

Para una prueba de banco con el nodo raíz:

```bash
python herramientas/correr_conector.py conectores/E1-PB-LECA-HFR01 --hw-id mac-<MAC de la placa> --docker aura-mosquitto
```

Completar con el hw_id real leído en placa, sin subir MAC de producción. El
runtime dentro de AURA está pendiente según la base recibida; estas pruebas
comprueban el conector aislado.
