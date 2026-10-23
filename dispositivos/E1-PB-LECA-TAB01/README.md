# E1-PB-LECA-TAB01 — medidor de energía del tablero del LabECA

| | |
|---|---|
| **Ubicación** | Edificio 1, planta baja, LabECA — dentro del tablero eléctrico |
| **Responsable** | Grupo de IC III 2026: francobroggi, tomasvidela11, santicianflone |
| **Estado** | en desarrollo |
| **Instalado** | todavía no |
| **Transporte** | mesh ESP-WIFI-MESH (interior), en ESP-IDF 6.1, según [ADR-007](../../docs/adr/ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.1 |
| **Basado en** | [`Mesh_Red_Completa/Mesh3_Hoja`](../../Mesh_Red_Completa/Mesh3_Hoja/), sobre la biblioteca `nodo_mesh` |
| **Códigos anteriores** | ninguno |

## Qué hace

Lee por Modbus RTU (RS-485) el medidor de energía Schneider EasyLogic PM2130 del tablero del
LabECA y envía a AURA las mediciones eléctricas de la fase A: tensión, corriente, potencia
activa y cos φ. El conector está en [`conectores/E1-PB-LECA-TAB01/`](../../conectores/E1-PB-LECA-TAB01/).

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | ESP32-C3 SuperMini, `hw_id` en la etiqueta de la placa (herramientas/leer_mac) | — |
| Transceptor RS-485 | Módulo MAX485 (DI / DE / RE / RO), DE y RE puenteados | DI ← GPIO21, RO → GPIO20, DE+RE ← GPIO5, VCC 3V3, GND |
| Medidor | Schneider EasyLogic PM2130 | A → D1, B → D0, GND → 0V. Modbus RTU, esclavo 1, 19200 baudios, 8E1 |
| Transformador de corriente | 100/5 A, una pasada del conductor | al PM2130 (relación configurada 100/5) |

## Lo que envía a AURA

Cada medición es un campo de `values` (contrato §3.1). Un valor que el medidor no entrega, que no
se pudo leer o que está fuera de su rango no aparece.

| Campo | Unidad | Rango válido | Se envía |
|---|---|---|---|
| `v_an_v` | V | 0 a 300 | siempre, cada `intervalo_s` |
| `i_a_a` | A | 0 a 120 | siempre |
| `p_w` | W | −30 000 a 30 000 | siempre |
| `cosphi_a` | — | 0 a 1 | siempre |
| `thd_v_an_pct` / `thd_i_a_pct` | % | 0 a 100 / 0 a 500 | si `ENVIAR_THD` = 1 (`main/pm2130.h`) |
| `tanphi` | — | −10 a 10 | si `ENVIAR_TANFI` = 1 |
| `e_act_kwh` | kWh | 0 a 10⁹ | si `ENVIAR_ENERGIA` = 1 |

El LabECA es monofásico: las fases B y C están en el firmware (`ENVIAR_FASES_BC`), pero no se
envían. Si todo junto no entra en 160 bytes, los opcionales salen en una segunda medición.

Alertas que emite (contrato §3.5): `sensor` para cada campo obligatorio, una vez por cambio, con
motivo `sin_respuesta` (el medidor no contesta o no entrega el valor) o `fuera_de_rango`.
No acepta comandos propios.

## Configuración

Parámetros que acepta `set_config` (contrato §3.3). Se validan todos antes de aplicar alguno y se
guardan en NVS. `aula` la maneja la biblioteca.

| Parámetro | Rango | Valor en esta placa |
|---|---|---|
| `intervalo_s` | 2 a 3600 (entero) | 10 |
| `id_esclavo` | 1 a 247 (entero) | 1 |

## Compilar y probar

1. ESP-IDF 6.1 (instalador EIM). Copiar `main/config_local.ejemplo.h` como `main/config_local.h` y
   completar el ID y la clave de la mesh, que da la cátedra (no se sube al repositorio).
2. Desde esta carpeta: `idf.py set-target esp32c3` y `idf.py -p <puerto> build flash monitor`.
3. Tests de la lógica del medidor (trama, validación, conversión, rangos), sin placa:
   `make -C tests`.

## Notas

- Al armar el banco, el PM2130 tenía la relación del TI en el valor de fábrica (5/5), lo que
  daba corrientes, potencias y energías 20 veces menores. Se configuró 100/5 y quedó guardado.
- Se detectó la polaridad del TI invertida (potencia activa negativa). Se corrige intercambiando
  S1 y S2 con el circuito **sin tensión**: nunca abrir el secundario de un TI con corriente.
- En el módulo MAX485, A y B estaban rotulados al revés respecto del PM2130: si no hay respuesta
  por Modbus, lo primero es intercambiarlos.
- La consola va por el USB del chip (`sdkconfig.defaults`), así la UART0 no usa los pines 20 y 21
  del RS-485.
