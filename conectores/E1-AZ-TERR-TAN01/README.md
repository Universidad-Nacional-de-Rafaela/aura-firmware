# E1-AZ-TERR-TAN01 — conector

Conector del dispositivo [`E1-AZ-TERR-TAN01`](../../dispositivos/E1-AZ-TERR-TAN01/): nivel de dos
tanques de agua de la terraza, por LoRaWAN (`hw_id` `eui-<DevEUI>`).

| Qué | Dónde | Cómo |
|---|---|---|
| Calcular | `nivel_y_volumen()` | `nivel_tN_pct` y `vol_tN_l` a partir de `dist_tN_cm` (lo único que manda el nodo), con las medidas de `TANQUES`; acotado a 0-100 % |
| Guardar | `al_recibir_datos()` | la distancia, el nivel y el volumen de cada tanque, con los rangos de `conector.toml`; si la distancia falta o está fuera de rango, de ese tanque no se guarda nada |
| Alertar | `al_recibir_datos()` | `nivel_bajo` cuando un tanque baja del 20 % y `nivel_alto` cuando llega al 95 % (si la distancia es menor que la de lleno, avisa "lleno o rebalsando, o sensor obstruido"), y cuando vuelven; **una vez por cruce** y por tanque, comparando con la última medición guardada (`ctx.serie`) |

Las distancias llegan con los nombres que devuelve [`codec.js`](../../dispositivos/E1-AZ-TERR-TAN01/codec.js)
en el device profile de ChirpStack: si cambian ahí, cambian acá. Para otro tanque, o si una medida
estaba mal, se cambia `TANQUES` (y el `max` de `vol_tN_l` en `conector.toml` si cambia la capacidad).

Probarlo:

```bash
python3 -m unittest discover -s conectores/E1-AZ-TERR-TAN01/tests
herramientas/correr_conector.py conectores/E1-AZ-TERR-TAN01 --hw-id eui-<DevEUI> --docker aura-mosquitto
```

El segundo necesita el `lorawan-bridge` publicando en el broker de AURA.
