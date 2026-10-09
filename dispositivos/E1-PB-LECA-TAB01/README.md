# E1-PB-LECA-TAB01 — medidor de energía del tablero del LabECA

| | |
|---|---|
| **Ubicación** | Edificio 1, planta baja, LabECA — dentro del tablero eléctrico |
| **Responsable** | Grupo de IC III 2026: francobroggi, tomasvidela11, santicianflone |
| **Estado** | en desarrollo |
| **Instalado** | todavía no |
| **Transporte** | mesh ESP-WIFI-MESH (interior), según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) y [ADR-006](../../docs/adr/ADR-006-esp-wifi-mesh-reemplaza-a-esp-now-en-interior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v4.0 |
| **Basado en** | ninguno |
| **Códigos anteriores** | ninguno |

## Qué hace

Lee por Modbus RTU (RS-485) el medidor de energía Schneider EasyLogic PM2130 del tablero del
LabECA y envía a AURA las mediciones eléctricas de la fase A: tensión, corriente, potencia
activa y cos φ. El conector está en [`conectores/E1-PB-LECA-TAB01/`](../../conectores/E1-PB-LECA-TAB01/).

## Hardware

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | Seeed XIAO ESP32S3, `hw_id` `TODO: mac-…` (herramientas/leer_mac) | — |
| Transceptor RS-485 | Módulo MAX485 (DI / DE / RE / RO) | DI → D6 (GPIO43), RO → D7 (GPIO44), DE + RE → D3 (GPIO4), VCC → 3V3 |
| Medidor | Schneider EasyLogic PM2130 | A → D1, B → D0, GND → 0V. Modbus RTU, esclavo 1, 19200 baudios, 8E1 |
| Transformador de corriente | 100/5 A, una pasada del conductor | al PM2130 (relación configurada 100/5) |

## Lo que envía a AURA

Cada medición es un campo de `values` (contrato §3.1). Un valor que el medidor no entrega, o que
no se pudo leer, no aparece.

| Campo | Unidad | Rango válido | Cada cuánto |
|---|---|---|---|
| `v_an_v` | V | 0 a 300 | TODO: a acordar con la cátedra, configurable |
| `i_a_a` | A | 0 a 120 | ídem |
| `p_w` | W | −30 000 a 30 000 | ídem |
| `cosphi_a` | — | 0 a 1 | ídem |
| `tanphi` | — | −10 a 10 | opcional |
| `thd_v_an_pct` | % | 0 a 100 | opcional |
| `thd_i_a_pct` | % | 0 a 500 | opcional |
| `e_act_kwh` | kWh | 0 a 10⁹ | opcional |

El aula es monofásica: las fases B y C están en el firmware, pero no se envían.

Alertas que emite (contrato §3.5): `sensor` cuando el medidor no responde por Modbus
(TODO: definir con la cátedra si es una alerta por campo o una sola).

## Configuración

Parámetros que acepta `set_config` (contrato §3.3).

| Parámetro | Rango | Valor en esta placa |
|---|---|---|
| `intervalo_s` | TODO | TODO |

## Compilar y probar

Firmware pendiente: se suma en otro pull request, cuando la mesh esté probada en placa.

## Notas

- Al armar el banco, el PM2130 tenía la relación del TI en el valor de fábrica (5/5), lo que
  daba corrientes, potencias y energías 20 veces menores. Se configuró 100/5 y quedó guardado.
- Se detectó la polaridad del TI invertida (potencia activa negativa). Se corrige intercambiando
  S1 y S2 con el circuito **sin tensión**: nunca abrir el secundario de un TI con corriente.
- En el módulo MAX485, A y B estaban rotulados al revés respecto del PM2130: si no hay respuesta
  por Modbus, lo primero es intercambiarlos.
