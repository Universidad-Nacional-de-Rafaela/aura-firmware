# E1-PB-LECA-HFR01 — heladera-freezer del LabECA

Monitoreo de temperatura de una heladera y de su freezer, con una sola placa y dos sondas.

| | |
|---|---|
| **Ubicación** | Edificio 1, planta baja, LabECA |
| **Responsable** | Grupo de IC IV 2026 |
| **Estado** | en desarrollo: el firmware lo sube el grupo a esta carpeta |
| **Instalado** | todavía no |
| **Transporte** | mesh ESP-NOW, según [ADR-003](../../docs/adr/ADR-003-dos-transportes-mesh-interior-lorawan-exterior.md) |
| **Contrato que implementa** | [`CONTRATO_MQTT.md`](../../docs/CONTRATO_MQTT.md) v3.0 |
| **Basado en** | ninguno |
| **Códigos anteriores** | ninguno |

## Lo que envía a AURA

| Campo | Unidad | Cada cuánto |
|---|---|---|
| `temp_heladera_c` | °C | a definir con el grupo |
| `temp_freezer_c` | °C | a definir con el grupo |
