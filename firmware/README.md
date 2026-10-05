# Firmware

Un firmware por carpeta: `Fnnn_<descripcion>/`, con el `.ino` llamado igual que la carpeta
(Arduino lo exige). **`Fnnn` es único y no se reutiliza nunca**, aunque un firmware se retire.
La descripción es libre y corta, en minúsculas con guion bajo.

Un firmware puede correr en muchas placas: dónde está instalada cada una lo dicen las fichas de
[`../dispositivos/`](../dispositivos/). Para empezar uno nuevo, copiá
[`../ejemplos/plantilla_firmware/`](../ejemplos/plantilla_firmware/) y tomá el número que sigue
en esta tabla, en el mismo PR.

**Extender antes que duplicar.** Si otro dispositivo necesita un firmware existente con un
cambio chico de hardware (otra pantalla, otro pin), se agrega como opción de `config_local.h`
en ese firmware. Un número nuevo, solo si el código es sustancialmente otro.

| Número | Carpeta | Qué hace | Estado | Dispositivos que lo usan |
|---|---|---|---|---|
| `F001` | `F001_temperatura_dos_sondas/` | Temperatura con dos sondas DS18B20, RTC y pantalla | reservado: lo sube el grupo de IC IV 2026 | `E1-PB-LECA-HFR01` |
