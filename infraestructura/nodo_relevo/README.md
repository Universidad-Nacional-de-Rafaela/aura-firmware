# nodo_relevo

Placa de la cátedra que extiende la mesh ESP-WIFI-MESH de un edificio: se une como nodo
intermedio (`MESH_NODE`) y reenvía el tráfico de las hojas hacia el raíz. No tiene lógica de
AURA ni tabla de hijos: la mesh arma y repara las rutas sola.

Se pone uno donde las hojas no llegan al raíz (otro piso, otro ala). Los dispositivos de los
grupos son hojas y **no** reenvían: por eso la cobertura la dan los relevos.

Configuración: `config_local.h` a partir de `config_local.h.example` (ID, clave y canal de la
mesh). No lleva la clave del WiFi del edificio.

El monitor serie muestra cada 30 s si está unido, en qué capa y cuántos nodos ve la mesh.
