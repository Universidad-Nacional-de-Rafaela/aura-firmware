# Conector de E1-PB-LECA-PRP01

Presencia de personas en el aula, detectada por una Raspberry Pi con cámara y publicada por
la hoja de la mesh [`dispositivos/E1-PB-LECA-PRP01`](../../dispositivos/E1-PB-LECA-PRP01/),
entregada en la [PR #60](https://github.com/Universidad-Nacional-de-Rafaela/aura-firmware/pull/60).

| Llega en `values` | Se guarda |
|---|---|
| `"presencia": 1` / `0` (enteros) | `presencia` = 1 / 0 |
| `"conf_pct": 0..100`, junto con presencia válida | `conf_pct` tal cual; si falta, se guarda solo presencia |
| `conf_pct` de tipo inválido o fuera de rango | se quita; la presencia válida se conserva |
| sin `presencia` | nada (`sin_presencia`) |
| `presencia` distinta de los enteros 0/1, incluidos `true`, `false`, `0.0` y `1.0` | nada (`presencia_invalida`) |

Las mediciones conservan su fecha e identificador de ingesta. Una muestra sin presencia válida
no se reemplaza por un cero, aunque antes se haya informado presencia.

La hoja publica en cada cambio y, si no cambia el estado, cada `intervalo_s` (300 s por defecto,
de 30 a 3600). El paso de 1 a 0 espera `delay_off_s` (30 s por defecto, de 0 a 600). Esa lógica
la resuelve la hoja: el conector guarda cada medición válida que recibe, sin agregar retardos.
Durante el retardo, la hoja puede omitir `conf_pct` si no corresponde al estado publicado.

No genera alertas propias: si la Pi manda `U` o pasan 30 s sin una línea válida, la hoja deja de
publicar mediciones y avisa con la alerta `sensor`; al volver, informa `recuperada`.

```bash
python3 -m unittest discover -s conectores/E1-PB-LECA-PRP01/tests -v
python3 herramientas/validar_conectores.py
```

Los cuatro mensajes de ejemplo deben dar tres mediciones guardadas y una descartada.
Para comprobarlo como corre en AURA, ejecutar en Linux:

```bash
python3 herramientas/correr_conector.py conectores/E1-PB-LECA-PRP01 --archivo conectores/E1-PB-LECA-PRP01/tests/mensajes_ejemplo.jsonl --subproceso
```
