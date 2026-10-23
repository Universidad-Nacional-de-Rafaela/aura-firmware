# Conector de E2-AZ-TERR-TAN01 — nivel de 3 tanques de agua

Conector del dispositivo [`E2-AZ-TERR-TAN01`](../../dispositivos/E2-AZ-TERR-TAN01/): tres sensores
ultrasónicos AJ-SR04M que miden la distancia al agua de tres tanques de la terraza del Edificio 2.
Va por LoRaWAN (`hw_id` `eui-<DevEUI>`).

| Qué | Cómo |
|---|---|
| Guardar | `distancia_tanqueN_cm` (cm con un decimal, de 25 a 600) y, si el tanque está calibrado, `nivel_tanqueN_pct` (0 a 100 %). Cada mensaje trae un tanque (el de ese turno) o los tres (después de `medir_ahora`). Lo que está fuera de rango lo quita AURA. `25` cm significa "25 cm o menos": el agua en la zona ciega del sensor, tanque lleno |
| Alertar | `tanque_bajo`: `warning` cuando el nivel baja de `NIVEL_MINIMO_PCT` (20 %) e `info` cuando vuelve a superar el mínimo + `HISTERESIS_PCT` (25 %), una vez por cruce y por tanque. Necesita el tanque calibrado (cero y lleno, por `set_config` desde AURA) |
| Alertar | `sensor`: `warning` cuando un tanque no llega en `MENSAJES_SIN_DATOS` (6) mensajes seguidos de los otros tanques (el nodo no manda un tanque cuyo sensor no midió) e `info` cuando vuelve. No depende del intervalo |
| Predecir | todavía no |

El dispositivo solo mide: la bomba la maneja un control externo, y `tanque_bajo` avisa si no llenó
a tiempo. El cero y el lleno de cada tanque se configuran en el nodo, no acá: ver la
[ficha](../../dispositivos/E2-AZ-TERR-TAN01/README.md#comandos).

Probarlo, desde la raíz del repo:

```bash
python -m unittest discover -s conectores/E2-AZ-TERR-TAN01/tests
python herramientas/correr_conector.py conectores/E2-AZ-TERR-TAN01 --archivo mensajes.jsonl
```
