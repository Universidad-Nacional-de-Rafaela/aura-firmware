# E1-PB-LECA-LUZ01 — control de iluminación del aula (LabECA)

Conector del dispositivo `E1-PB-LECA-LUZ01`: recibe la presencia que manda la placa y la
guarda en AURA, alertando una sola vez por cruce de estado.

| Archivo | Qué es |
|---|---|
| `conector.toml` | manifiesto: nombre, autores, campos con su rango |
| `conector.py` | la clase `ConectorLuz01`: `al_recibir_datos()` |
| `tests/test_conector.py` | tests con `ContextoDePrueba` (sin AURA ni placa) |

## Campos (etapa 1)

| Campo | Unidad | Rango | Qué es |
|---|---|---|---|
| `presencia` | 0/1 | 0 – 1 | 1 = hay alguien en el aula (LD2420) |

Etapa 2 (cuando se conecten el BH1750 y el relé): `luz_ventana_lx`, `luz_aula_lx`,
`rele_canal1`, `rele_canal2` — declarados en el manifiesto en el mismo PR que los manda el
firmware.

## Qué hace

- `guardar_medicion()` con lo que esté dentro de rango; si nada es válido, la muestra se
  descarta (`sin_campos_validos`).
- **Alerta `presencia` (severidad `info`) una sola vez por cruce**: `"Aula ocupada"` al
  pasar de 0 a 1, `"Aula libre"` al volver a 0. No alerta en cada medición. El primer dato
  que llega con `presencia = 1` también alerta (no hay estado anterior que comparar);
  con `presencia = 0` no alerta.

## Lo que NO hace

La decisión de prender o apagar la luz **no pasa por el conector**: la toma la regla local del
firmware (`dispositivos/E1-PB-LECA-LUZ01/regla_luz.h`). El conector solo deja la serie y las
alertas en AURA.

## Tests

```bash
python3 -m unittest discover -s conectores/E1-PB-LECA-LUZ01/tests -v
```
