# Conector de E1-PB-LECA-CRT01 — Cortina automatizada Luflex

Procesa las mediciones de la cortina automatizada (`posicion_pct`, `luz_lux`, `estado_motor` y `modo`), descarta valores fuera de rango físico y emite la alerta `motor_atascado` (`high`) si el motor permanece en movimiento o calibración durante 180 s o más, cerrándola con `info` cuando vuelve a detenerse (`0`).

## Correr los tests

Desde la raíz del repositorio:

```bash
python3 -m unittest discover -s conectores/E1-PB-LECA-CRT01/tests -v
```
