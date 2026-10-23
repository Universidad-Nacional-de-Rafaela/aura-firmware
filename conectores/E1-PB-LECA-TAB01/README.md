# E1-PB-LECA-TAB01 — conector del medidor de energía del LabECA

Procesa lo que manda el medidor Schneider PM2130 del tablero del LabECA (leído por Modbus RTU
desde una XIAO, hoja de la mesh): guarda las mediciones eléctricas de la fase A y alerta cuando
la instalación sale de lo normal.

## Campos

Tienen que coincidir con lo que manda el firmware (`values`, contrato §3.1).

| Campo | Unidad | Rango | |
|---|---|---|---|
| `v_an_v` | V | 0 a 300 | obligatorio |
| `i_a_a` | A | 0 a 120 | obligatorio (TI 100/5 A) |
| `p_w` | W | −30 000 a 30 000 | obligatorio |
| `cosphi_a` | — | 0 a 1 | obligatorio |
| `tanphi` | — | −10 a 10 | opcional; + inductivo, − capacitivo |
| `thd_v_an_pct` | % | 0 a 100 | opcional |
| `thd_i_a_pct` | % | 0 a 500 | opcional |
| `e_act_kwh` | kWh | 0 a 10⁹ | opcional |

Las fases B y C están en el manifiesto, comentadas: el aula es monofásica y el firmware no las
envía.

## Alertas

Una por cruce del umbral y otra (`info`) al volver a lo normal. Para saber cómo estaba, compara
con la última medición guardada de la última hora (`ctx.serie`).

| Tipo | Severidad | Cuándo |
|---|---|---|
| `subtension` | warning | tensión por debajo de 198 V (220 V − 10 %) |
| `sobretension` | warning | tensión por encima de 242 V (220 V + 10 %) |
| `sobrecorriente` | high | corriente por encima del 80 % de la protección del circuito |
| `potencia_negativa` | warning | potencia activa por debajo de −50 W: en un aula, TI invertido |
| `bajo_cosphi` | warning | cos φ por debajo de 0,85 con al menos 200 W de carga |

Los umbrales son una **propuesta** y están arriba de todo en `conector.py`. Falta el dato de la
protección del circuito (`TERMICA_A`).

Un campo que falta en una muestra no activa ni normaliza ninguna alerta: no se sabe. Un valor
fuera de rango tampoco: AURA lo quita antes.

## Probarlo

```bash
python3 -m unittest discover -s conectores/E1-PB-LECA-TAB01/tests
herramientas/correr_conector.py conectores/E1-PB-LECA-TAB01 --hw-id mac-<MAC de la placa> --docker aura-mosquitto
```
