# Conector de E2-AZ-TERR-TAN01 — nivel de 3 tanques de agua

Conector del dispositivo [`E2-AZ-TERR-TAN01`](../../dispositivos/E2-AZ-TERR-TAN01/): tres sensores
ultrasónicos AJ-SR04M que miden la distancia al agua de tres tanques de la terraza del Edificio 2.
Va por LoRaWAN (`hw_id` `eui-<DevEUI>`).

| Qué | Cómo |
|---|---|
| Guardar | `distancia_tanque1_cm`, `distancia_tanque2_cm` y `distancia_tanque3_cm`, en cm con un decimal, de 25 a 600. Lo que está fuera de ese rango lo quita AURA. `25` significa "25 cm o menos": el agua en la zona ciega del sensor, tanque lleno |
| Alertar | `tanque_bajo`: `warning` cuando a un tanque le queda menos del 20 % de agua (`NIVEL_MINIMO`) e `info` cuando sube más de 5 cm (`HISTERESIS_CM`) por encima de ese nivel, una vez por cruce y por tanque. El umbral sale de `ALTURA_CM`, la distancia del sensor al fondo de cada tanque |
| Alertar | `sensor`: `warning` cuando el campo de un tanque falta en un mensaje que trae otro (el codec no manda el de un sensor que falla) e `info` cuando vuelve, una vez por falla y por tanque |
| Predecir | todavía no |

**Pendiente:** `ALTURA_CM` en `conector.py` tiene valores de prueba (163 cm, umbral 130,4 cm).
Hay que medir la distancia del sensor al fondo de cada tanque y poner los valores reales. El
dispositivo solo mide: la bomba la maneja un control externo, y `tanque_bajo` avisa si no llenó a
tiempo.

Probarlo, desde la raíz del repo:

```bash
python -m unittest discover -s conectores/E2-AZ-TERR-TAN01/tests
python herramientas/correr_conector.py conectores/E2-AZ-TERR-TAN01 --archivo mensajes.jsonl
```
