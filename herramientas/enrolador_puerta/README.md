# enrolador_puerta — enroladora de tarjetas de la puerta

Herramienta de banco para la puerta [`E1-PB-LECA-PUE01`](../../dispositivos/E1-PB-LECA-PUE01/):
agrega y borra tarjetas autorizadas, y sirve de radio para mandarle órdenes desde la compu.
No es un dispositivo de AURA: no manda datos al contrato.

## Qué hace

- **Pasar una tarjeta** la agrega a la lista de la puerta; con el botón apretado (GPIO 4) la
  **borra**. Le manda el UID por ESP-NOW cifrado (canal 1) y suena según la respuesta: moneda =
  agregada, cuatro notas bajando = borrada, dos pitidos cortos = ya estaba / no estaba, dos
  graves = error o sin respuesta.
- **Modo gateway por USB**: por el puerto serie acepta líneas `GW <n> <mac12hex> <comando>` con
  `abrir` o `emergencia` y le pasa la orden a la puerta por ESP-NOW. Contesta `GW_TX`, `GW_OK` o
  `GW_ERR <n> <motivo>`. Sirve para simular el camino app → broker → gateway → placa, con un
  script en la compu que traduzca `hw/<hw_id>/command` a estas líneas.

## Hardware

Misma placa y cableado que la puerta, sin relé ni cerradura:

| Componente | Modelo | Conexión |
|---|---|---|
| Placa | ESP32-C3 SuperMini | — |
| Lector RFID | RC522 | SCK 9, MISO 10, MOSI 20, SS 21, RST 7 (3,3 V) |
| Buzzer pasivo | directo al pin | GPIO 3 |
| Botón de borrar | entre GPIO 4 y GND (pull-up interna) | GPIO 4 |

## Compilar

1. Copiar `config_local.h.example` como `config_local.h` y completarlo con la MAC de la puerta
   y las claves de ESP-NOW (las mismas que en la puerta). Sin él compila, pero no se empareja
   con ninguna puerta.
2. Bibliotecas: las de `bibliotecas.txt`.
3. Placa `ESP32C3 Dev Module` o `Nologo ESP32C3 Super Mini` con `CDCOnBoot=cdc`.
