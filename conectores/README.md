# Conectores

Un **conector** es el código que procesa, dentro de AURA, lo que manda un dispositivo: valida y
guarda sus mediciones, alerta cuando hace falta y, si quiere, predice. Cada grupo escribe el de su
dispositivo. Corre aislado: lo único que puede tocar es el `Contexto` que le da AURA, y no tiene
acceso a la base ni a la red.

```
dispositivo ──► raíz de la mesh / lorawan-bridge ──► hw/<hw_id>/data ──► AURA ──► tu conector
                                                                          │            │
                                                                          ◄── ack ─────┘ (guardó o descartó)
```

**Una carpeta por conector**, con el **código del dispositivo** (el mismo que su carpeta en
[`../dispositivos/`](../dispositivos/)):

```
conectores/E1-PB-LECA-HFR01/
├── conector.toml      # manifiesto: campos con su rango, autores, si predice
├── conector.py        # la clase: al_recibir_datos() y, opcional, predecir()
├── README.md
└── tests/test_conector.py
```

Para arrancar, copiá [`../ejemplos/plantilla_conector/`](../ejemplos/plantilla_conector/). El
ejemplo completo es [`../ejemplos/conector_ejemplo/`](../ejemplos/conector_ejemplo/). La guía está
en [`CONTRIBUTING.md`](../CONTRIBUTING.md), "Tu conector", y la biblioteca en
[`../comun/python/aura_sdk/`](../comun/python/aura_sdk/).

> **Estado (2026-10-07).** El SDK, los ejemplos y `herramientas/correr_conector.py` funcionan:
> cada grupo puede escribir y probar su conector en el banco. El **runtime** que los corre dentro
> de AURA todavía no existe; cuando exista, corre los mismos conectores sin cambios.
