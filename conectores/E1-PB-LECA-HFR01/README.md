# Conector de heladera y freezer IC IV

Procesa las mediciones de una placa con hw_id y dos campos independientes:
temp_heladera_c / temp_freezer_c. Python 3.11 o posterior; usa exclusivamente
el SDK aura_sdk de comun/python/, sin paquetes externos ni acceso a red/archivos.

| Archivo | Función |
|---|---|
| conector.toml | Código, usuarios del grupo, unidad y rango físico de las dos sondas. |
| conector.py | Validación, guardado y alertas por cruce de umbrales. |
| tests/test_conector.py | 20 pruebas con ContextoDePrueba, sin AURA ni hardware. |
| tests/mensajes_ejemplo.jsonl | Seis mensajes ficticios para probar el runner por archivo. |

## Decisiones

Solo guarda temperaturas numéricas finitas entre -55 y 125 °C. Omite 85 °C,
-127, null, booleanos, cadenas, campos desconocidos y diagnóstico. Si una sonda
es válida y la otra falla, guarda solo la válida. Si no queda ninguna, descarta
con motivo sin_temperaturas_validas. Conserva msg.ts; si falta, el SDK asigna
hora de llegada. No inventa una hora de medición.

El runtime deduplica ingest_id antes de invocar el conector. Si el conector lanza
una excepción, se aplica la regla D5: se deshacen sus operaciones parciales,
se guarda la muestra cruda conservando solo los campos válidos según el
manifiesto y se genera la alerta conector_fallo. AURA responde con ACK
persistido, o descartado si no quedó ningún campo válido, en ambos casos con
motivo conector_fallo. El dispositivo no reenvía esa muestra tras recibir el ACK.
El rescate aplica el manifiesto, sin ejecutar los filtros propios del conector,
como la exclusión del centinela 85 °C.

El conector no publica ACK por su cuenta: decide guardar/descartar con el
Contexto. Los fallos transitorios de AURA, como una base de datos no disponible,
son distintos de una falla del conector y pueden dejar la muestra sin ACK para
que el dispositivo reintente.

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

Después de sincronizar la rama con main, probar también con el runner aislado
que usa AURA, sobre Linux (o una distribución Linux de WSL):

```bash
python3 herramientas/correr_conector.py conectores/E1-PB-LECA-HFR01 --archivo conectores/E1-PB-LECA-HFR01/tests/mensajes_ejemplo.jsonl --subproceso
```

El archivo contiene seis mensajes ficticios: lecturas normales, un cruce de
umbral y su recuperación, un duplicado, una muestra inválida y un centinela de
sonda. Resultado esperado: 4 persistidos, 1 descartado y 1 duplicado, con una
alerta de temperatura de heladera y otra de recuperación. No se publica ningún
ACK real ni se modifica AURA; el contexto del runner vive en memoria.

El aislamiento de esta versión del SDK utiliza límites de procesos de Linux;
`--subproceso` no se valida ejecutándolo directamente en Windows. Los tests
unitarios anteriores y el runner sin ese parámetro sirven para probar la lógica
en Windows, pero no equivalen a la prueba aislada. La regla D5 ante excepciones
se verifica en los tests del SDK.

Para una prueba de banco con el nodo raíz:

```bash
python herramientas/correr_conector.py conectores/E1-PB-LECA-HFR01 --hw-id mac-<MAC de la placa> --docker aura-mosquitto
```

Completar con el hw_id real leído en placa, sin subir MAC de producción.
El runner comprueba el conector sin persistencia real en AURA. El runtime de
conectores ya está disponible en AURA; para habilitar uno nuevo, la cátedra debe
actualizar su submódulo a un commit que lo incluya y asignarlo al dispositivo.
La prueba integral requiere el alta de la placa y el raíz con el broker autenticado.
La herramienta del banco debe estar actualizada con el soporte de usuario y clave
publicado por la cátedra; las credenciales reales no se agregan al repositorio.
