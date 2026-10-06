# E1-PB-LECA-HFR01 — Monitoreo de heladera y freezer

| | |
|---|---|
| **Ubicación** | Propuesta: Edificio 1, planta baja, LabECA; confirmar instalación en clase. |
| **Responsable** | Grupo IC IV 2026; VainstubTomas, Bernidb, Kinggrass265, JuanCruz46 |
| **Estado** | En desarrollo, compilado y probado en host; pendiente de banco físico. |
| **Instalado** | Todavía no. |
| **Transporte** | Mesh ESP-NOW, interior; sensor → sala → gateway. |
| **Contrato que implementa** | CONTRATO_MQTT.md v3.0, publicado como propuesta; adaptación central pendiente. |
| **Basado en** | Encadenamiento de aura-firmware y desarrollo IC IV. |
| **Códigos anteriores** | Ninguno confirmado. |

## Qué hace

Una XIAO lee dos sondas DS18B20 independientes, evalúa umbrales localmente,
enciende LED de alarma/desconexión y conserva una cola durante cortes.
La placa tiene **un UUID**; cada sonda tiene su nombre de medición.

## Organización del código

El sketch `E1-PB-LECA-HFR01.ino` carga `app.h`, que coordina el arranque
y el ciclo de ejecución. La lógica está separada por responsabilidad:

| Archivo | Responsabilidad |
|---|---|
| `app.h` | Orden de inicialización y llamadas del ciclo principal. |
| `configuracion.h` | Valores por defecto de pines, MAC, canal y RTC; admite `config_local.h`. |
| `estado_nodo.h` | Objetos de hardware y estado compartido entre módulos. |
| `sondas.h` | Lecturas independientes de los dos DS18B20, validación y alertas de falla/recuperación. |
| `pantalla.h` | OLED, LED de umbral y LED de desconexión. |
| `reloj.h` | Inicialización y validación del RTC; fecha de medición en UTC. |
| `energia.h` | Detección de batería/red y captura del inicio del corte. |
| `almacenamiento.h` | Carga y guardado del journal en NVS; IDs de eventos. |
| `comunicacion.h` | Envíos ESP-NOW, ACK, reintentos, barrido de recuperación y comandos remotos. |
| `mesh_core.h` | Formato de tramas, FIFO, validación de configuración y reglas compartidas. |
| `mesh_radio.h` | Adaptador ESP-NOW y cola de recepción del callback. |
| `mesh_storage.h` | Implementación NVS por registros, índices y CRC. |

Todos los módulos están junto al sketch y se editan en esta carpeta. Se usan
headers para conservar la compilación como un solo sketch y la compatibilidad
con el generador de `autocontenido/` de AURA, que copia los archivos de la carpeta.
`config_local.h` sigue siendo local e ignorado por Git. `tests/` contiene
las pruebas del nodo y no se carga en la placa.

## Hardware

| Componente | Modelo | Conexión inicial configurable |
|---|---|---|
| Placa | Seeed XIAO ESP32S3, flash 8 MiB | ESP-NOW |
| Sonda heladera | DS18B20 | D2, bus 1-Wire propio y pull-up |
| Sonda freezer | DS18B20 | D3, bus 1-Wire propio y pull-up |
| LED umbral | LED con resistencia | D0 |
| LED desconexión | LED con resistencia | D1 |
| Pantalla | OLED SH1106 128×64 | I2C D4/D5 |
| Reloj | RTC DS3231 | I2C D4/D5 |
| Alimentación | Módulo batería del grupo, circuito pendiente | Entrada deshabilitada −1; definir nivel 3,3 V |

## Lo que envía a AURA

El gateway mapea la MAC a un UUID y envía REST, un evento por solicitud,
tipo `temperatura`, ID estable y `ts` UTC del RTC cuando es válido.
No se envían mediciones por MQTT AURA ni se completa una sonda con la última
lectura de la otra. Falla, NaN, −127 y 85 °C se omiten como medición.

| Campo | Unidad | Rango válido | Cada cuánto |
|---|---|---|---|
| `temp_heladera_c` | °C | −55..125; 85 reservado como inválido | 60 s configurable |
| `temp_freezer_c` | °C | −55..125; 85 reservado como inválido | 300 s configurable |

Alertas MQTT normativas: `sensor`, con `details.campo` y motivo
sin_respuesta/fuera_de_rango/recuperada; `energia`, con alimentación bateria/red.
Se generan por transición. Configuración completa y contadores van en
`status.details`, no en temperaturas. Sin RTC no se inventa fecha de medición.

## Configuración

`set_config` lleva solo parámetros cambiados. El nodo combina, valida,
guarda y reporta el conjunto vigente. Rechazo atómico; confirma con `aplicado`.

| Parámetro | Rango | Valor inicial |
|---|---|---|
| `intervalo_heladera_s` | entero 5..86400 | 60 |
| `intervalo_freezer_s` | entero 5..86400 | 300 |
| `min_heladera_c` / `max_heladera_c` | −55..125, min < max, centésimas | 2 / 6 |
| `min_freezer_c` / `max_freezer_c` | −55..125, min < max, centésimas | −25 / −15 |
| `recuperacion_s` | entero 60..86400, común a la placa | 300 |

MAC, canal y circuito se definen en `config_local.h` desde la plantilla;
no hay secretos ni direcciones de producción versionadas. El UUID vive en
la tabla local del gateway. El ID de comando es texto opcional, máximo 64 bytes.

## Compilar y probar

1. Placa XIAO_ESP32S3, core ESP32 **3.3.11**, USB CDC On Boot Enabled.
2. Instalar las versiones de `bibliotecas.txt`. Abrir `E1-PB-LECA-HFR01.ino`.
3. Crear `config_local.h` desde el ejemplo y completar MAC/canal sin versionarlo.
4. Verificar tabla propia `partitions.csv`, NVS 128 KiB, flash 8 MiB. Si hubo
   firmware anterior, respaldar cola y configuración **antes de cambiar tabla**.
5. Monitor 115200: NVS OK, muestras, ACK de gateway y central. Valores de
   marcador permiten compilar, pero no comunicar con un entorno real.
6. En `tests/`, `make` ejecuta pruebas C++17 de FIFO/ACK/CRC y persistencia con
   fallos de escritura simulados, sin placa ni bibliotecas Arduino externas.
7. En banco: validar ambas sondas, LED umbral sin red, 3 reintentos y recuperación,
   rechazo de parches inválidos, reinicio conservando ajustes, corte con
   sondas ausentes, overflow y reenvío ordenado por hora original.

## Notas

Esta carpeta es la fuente del firmware: el código, los headers del protocolo y
los tests se editan acá. No hay otra copia de la que se genere.
Cada registro mide 34 bytes con CRC, índice 341 bytes; el ACK no reescribe la
FIFO. Capacidad: 30 ordinarios por sonda + primera captura protegida por sonda
+ inicio del corte. Alertas consumen FIFO. No se ha medido vida útil de flash.

**Requiere infraestructura compatible con el protocolo IC IV v3.** La base
recibida de aura-firmware usa v1 y no interpreta estas tramas. Los headers
están dentro de esta carpeta para que compile sin modificar `comun/`, pero eso
no hace compatible la radio de infraestructura. La adaptación de sala/gateway
(tramas v3 y tabla MAC → UUID de §2.4) va en un PR separado a `infraestructura/`;
este PR no modifica infraestructura.
Confirmar con cátedra UUID/código/ubicación, circuito de alimentación,
seguridad PMK/LMK, endpoint/autenticación y despliegue de `aplicado`/alertas.
ACK de alerta confirma publicación MQTT, no guardado durable en AURA.
En offline se espacia el envío y se apaga OLED; la radio sigue escuchando para
recibir comandos, como pide el contrato. Los comandos llegan solo desde AURA. La autonomía queda por medir en placa.

No versionar config_local.h, respaldos NVS ni ejecutables de tests.
