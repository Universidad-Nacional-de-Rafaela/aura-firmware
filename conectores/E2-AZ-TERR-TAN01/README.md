# Conector de E2-AZ-TERR-TAN01 — nivel de 3 tanques de agua

Conector del dispositivo [`E2-AZ-TERR-TAN01`](../../dispositivos/E2-AZ-TERR-TAN01/): tres sensores
ultrasónicos AJ-SR04M que miden la distancia al agua de tres tanques de la terraza del Edificio 2.
Va por LoRaWAN (`hw_id` `eui-<DevEUI>`).

| Qué | Cómo |
|---|---|
| Guardar | `distancia_tanque1_mm`, `distancia_tanque2_mm` y `distancia_tanque3_mm`, de 250 a 6000 mm. Lo que está fuera de ese rango lo quita AURA, incluidos los valores especiales del firmware cuando un sensor falla. `250` significa "250 mm o menos": el agua en la zona ciega del sensor, tanque lleno |
| Alertar | `tanque_bajo`: `warning` cuando a un tanque le queda menos del 20 % de agua (`NIVEL_MINIMO`) e `info` cuando sube más de 50 mm (`HISTERESIS_MM`) por encima de ese nivel, una vez por cruce y por tanque. El umbral sale de `ALTURA_MM`, la distancia del sensor al fondo de cada tanque |
| Alertar | `sensor`: `warning` cuando el campo de un tanque falta en un mensaje que trae otro (el codec no manda el de un sensor que falla) e `info` cuando vuelve, una vez por falla y por tanque |
| Predecir | todavía no |

**Pendiente:** `ALTURA_MM` en `conector.py` tiene valores de prueba (1630 mm, umbral 1304 mm).
Hay que medir la distancia del sensor al fondo de cada tanque y poner los valores reales. El
dispositivo solo mide: la bomba la maneja un control externo, y `tanque_bajo` avisa si no llenó a
tiempo.

Probarlo, desde la raíz del repo:

```bash
python -m unittest discover -s conectores/E2-AZ-TERR-TAN01/tests
python herramientas/correr_conector.py conectores/E2-AZ-TERR-TAN01 --archivo mensajes.jsonl
```
