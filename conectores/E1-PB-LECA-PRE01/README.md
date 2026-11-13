# Conector de E1-PB-LECA-PRE01

Presencia de personas en el aula, detectada por una Raspberry Pi con cámara y publicada por
la hoja de la mesh [`dispositivos/E1-PB-LECA-PRE01`](../../dispositivos/E1-PB-LECA-PRE01/).

| Llega en `values` | Se guarda |
|---|---|
| `"presencia": true` / `false` | `presencia` = 1 / 0 (AURA guarda solo números) |
| `"presencia": 1` / `0` (enteros) | `presencia` = 1 / 0 |
| `"conf_pct": 0..100` | `conf_pct` tal cual; fuera de rango lo quita AURA |
| sin `presencia`, o con un valor distinto de booleano o entero 0/1 | nada (`descartar`) |

No genera alertas propias: la falla de la cámara la avisa la hoja con la alerta `sensor`.

```bash
python3 -m unittest discover -s conectores/E1-PB-LECA-PRE01/tests
```
