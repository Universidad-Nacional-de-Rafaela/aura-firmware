# Conector de E2-AZ-TERR-TAN01 — nivel de 3 tanques de agua

Conector del dispositivo [`E2-AZ-TERR-TAN01`](../../dispositivos/E2-AZ-TERR-TAN01/): tres sensores
ultrasónicos AJ-SR04M que miden la distancia al agua de tres tanques de la terraza del Edificio 2.
Va por LoRaWAN (`hw_id` `eui-19e2db6c14a8178c`).

| Qué | Cómo |
|---|---|
| Guardar | `distancia_tanque1_mm`, `distancia_tanque2_mm` y `distancia_tanque3_mm`, de 250 a 6000 mm. Lo que está fuera de ese rango lo quita AURA, incluidos los valores especiales del firmware cuando un sensor falla |
| Alertar | `tanque_bajo`: `warning` cuando la distancia pasa `UMBRAL_BAJO_MM` y `info` cuando vuelve, una vez por cruce y por tanque |
| Predecir | todavía no |

**Pendiente:** `UMBRAL_BAJO_MM` en `conector.py` es un valor provisional. Hay que ajustarlo con la
altura real de cada tanque.

Probarlo, desde la raíz del repo:

```bash
python -m unittest discover -s conectores/E2-AZ-TERR-TAN01/tests
python herramientas/correr_conector.py conectores/E2-AZ-TERR-TAN01 --archivo mensajes.jsonl
```
