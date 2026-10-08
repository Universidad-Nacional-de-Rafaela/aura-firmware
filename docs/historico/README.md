# Histórico de la mesh ESP-NOW

La mesh de interior de AURA se construyó primero sobre **ESP-NOW**, con un protocolo propio de
la cátedra. Desde el 2026-10-07 se reemplaza por **ESP-WIFI-MESH**
([ADR-006](../adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md)); el diseño vigente
está en [`../mesh-wifi/`](../mesh-wifi/).

| Carpeta | Qué fue |
|---|---|
| [`mesh-v1/`](mesh-v1/) | Primera mesh ESP-NOW (contrato v1.x, agosto de 2026): diseño, plan y estado de banco, con los cinco defectos que aparecieron **solo en hardware** |
| [`mesh-v2/`](mesh-v2/) | Mesh ESP-NOW del contrato v3.0 (octubre de 2026): cola persistente en el nodo, `ingest_id` del nodo, confirmación de punta a punta |

El código de la última versión ESP-NOW (gateway, sala, biblioteca de nodo y ejemplos) está en
el tag **`mesh-espnow-v2`**:

```bash
git checkout mesh-espnow-v2
```

Lo que se conservó de esa versión en la mesh actual: la cola persistente, el `ingest_id`
generado en el nodo, la confirmación solo con la respuesta de AURA, las alertas de sonda una vez
por cambio, `set_config` validado y la API de `comun/nodo_mesh.h` para los grupos.
