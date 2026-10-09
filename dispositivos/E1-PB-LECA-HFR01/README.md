# E1-PB-LECA-HFR01 — Monitoreo de heladera y freezer

| | |
|---|---|
| **Ubicación** | Propuesta: Edificio 1, planta baja, LabECA; confirmar instalación en clase. |
| **Responsable** | Grupo IC IV 2026: VainstubTomas, Bernidb, Kinggrass265, JuanCruz46. |
| **Estado** | En desarrollo; validación de software y banco físico por separado. |
| **Instalado** | Todavía no. |
| **Transporte** | Interior: hoja ESP-WIFI-MESH de AURA; no reenvía tráfico de otras placas. |
| **Contrato** | docs/CONTRATO_MQTT.md v4.0, propuesta publicada el 07/10/2026. |
| **Basado en** | Plantilla de dispositivo y comun/nodo_mesh.h de aura-firmware; lógica propia IC IV. |
| **Códigos anteriores** | Ninguno confirmado. |

## Qué hace

Una XIAO ESP32S3 mide dos DS18B20, con intervalos y umbrales independientes,
controla la alarma local, muestra temperaturas y conserva lecturas pendientes.
La placa es una hoja: la mesh elige el camino al raíz de la cátedra. Su identidad
es `mac-` más la MAC STA de fábrica, en minúsculas y sin separadores. No configura
UUID de AURA, MAC del padre ni MAC del raíz. La cátedra asigna su hw_id en AURA.

## Organización del código

| Archivo | Responsabilidad |
|---|---|
| E1-PB-LECA-HFR01.ino / app.h | Entrada Arduino, arranque y ciclo principal. |
| configuracion.h / config_local.h.example | Pines y plantilla de datos privados de la mesh. |
| estado_nodo.h | Objetos de hardware y estado compartido. |
| sondas.h | Dos conversiones sin espera, intervalos independientes y fallas de sondas. |
| pantalla.h | OLED y ambos LEDs; alarma local sin depender de AURA. |
| reloj.h | RTC, UTC y reloj de sistema sincronizable por confirmaciones del raíz. |
| energia.h | Entrada configurable de batería/red, antirrebote y captura del corte. |
| almacenamiento.h / mesh_storage.h / mesh_core.h | Journal NVS, FIFO, configuración y primera captura; formato compatible con la versión anterior. No son el protocolo de radio. |
| modelo_envio.h / envio_persistente.h | Mensaje completo en vuelo y asociación de las dos lecturas de una misma medición. |
| adaptador_aura.h | JSON de mediciones y validación/configuración compacta. |
| politica_envio.h | Presupuesto de reintentos y recuperación espaciada. |
| comunicacion.h | Callbacks de configuración y adaptación a comun/nodo_mesh.h. |
| tests/ | Pruebas locales; no se cargan en la placa. |

La biblioteca compartida se incluye desde `../../comun/nodo_mesh.h`, sin
modificarla. Para Arduino IDE se puede generar `autocontenido/` con la herramienta
de AURA. Los módulos siguen separados en ambos casos.

## Hardware

| Componente | Modelo | Conexión inicial configurable |
|---|---|---|
| Placa | XIAO ESP32S3, flash 8 MiB | ESP-WIFI-MESH |
| Sonda heladera | DS18B20, bus 1-Wire con pull-up propio | D2 |
| Sonda freezer | DS18B20, bus 1-Wire con pull-up propio | D3 |
| LED umbral | LED con resistencia | D0 |
| LED desconexión | LED con resistencia | D1 |
| OLED SH1106 / RTC DS3231 | I2C | D4/D5 |
| Presencia de alimentación | Circuito del grupo pendiente | -1: entrada deshabilitada |

## Lo que envía a AURA

El raíz publica en `hw/<hw_id>/data` un objeto `values`, su `ingest_id` estable
y `ts` UTC cuando la placa tiene hora válida. No hay REST desde el dispositivo,
UUID ni `tenant_id` ni `type`. RSSI, colas, alimentación y configuración van en
estado, nunca entre las temperaturas. No se inventa un campo de una sonda que falla.

| Campo | Unidad | Rango físico | Valor inicial de intervalo |
|---|---|---|---|
| temp_heladera_c | °C | -55..125; 85 se omite como centinela | 60 s |
| temp_freezer_c | °C | -55..125; 85 se omite como centinela | 300 s |

Si ambas lecturas pendientes tienen el mismo timestamp válido y corresponden
al mismo tipo de captura, se agrupan en un mensaje. Si sus intervalos/horas son
distintos, se envían por separado; sin hora no se supone simultaneidad.

Una confirmación del raíz significa que AURA resolvió el ingest_id: puede ser
persistido, duplicado, cuarentena, descartado o rechazado. No significa siempre
que la temperatura se guardó con un dispositivo. La placa libera esos registros
solo con la confirmación del ID correspondiente, nunca por envío aceptado o PUBACK.

Falla/recuperación de sonda y alimentación usan las alertas normativas `sensor`
y `energia`, por transición. Las alertas de umbral las calcula el conector en
AURA. El LED evalúa ambas sondas localmente, incluso sin comunicación.

## Configuración

`set_config` recibe un parche: valida todo, guarda en NVS y confirma aplicado.
Los nombres son compactos para que el reporte completo entre en los 180 bytes
de la biblioteca. Son parámetros del dispositivo, no nuevos campos de medición.

| Parámetro | Significado y rango | Inicial |
|---|---|---|
| intervalo_s | Intervalo de heladera, entero 5..86400 s; el raíz lo usa para inferir offline | 60 |
| f_s | Intervalo de freezer, entero 5..86400 s | 300 |
| hlo / hhi | Mínimo/máximo de heladera, °C, -55..125, mínimo < máximo, resolución de centésima | 2 / 6 |
| flo / fhi | Mínimo/máximo de freezer, mismas reglas | -25 / -15 |
| r_s | Recuperación de envío sin confirmación, entero 60..86400 s | 300 |

```json
{"command":"set_config","params":{"intervalo_s":120,"f_s":600},"command_id":"iciv-1"}
```

Se conserva el intervalo de la otra sonda si no aparece. `intervalo_s` no fuerza
una medición del freezer. Usar command_id corto o UUID de 36 caracteres y parches
pequeños; el comando entero debe entrar en 180 bytes. La configuración completa
en el resultado positivo también entra con un ID de 36 caracteres. IDs más largos
pueden hacer que la biblioteca omita config del resultado; el reporte posterior
mantiene la configuración completa. Las limitaciones de integración se detallan en la sección Notas.

## Compilar y probar

1. Core Arduino ESP32 3.3.11, placa XIAO_ESP32S3, USB CDC On Boot Enabled.
2. Instalar las versiones de bibliotecas.txt; incluye ArduinoJson 7.4.3.
3. Conservar la estructura dispositivos/ y comun/ y abrir el .ino de esta carpeta.
   Alternativa: generar y abrir autocontenido/E1-PB-LECA-HFR01/.
4. Crear config_local.h desde el ejemplo, sin pisar un archivo existente. Pedir
   MESH_ID, MESH_CLAVE y canal a la cátedra. Sin esos datos mide y guarda, no comunica.
5. Conservar partitions.csv, NVS 128 KiB y flash 8 MiB. Respaldar NVS/LittleFS si
   la placa ya tiene datos. No cambiar tabla ni borrar flash para resolver un error.
6. Monitor 115200: hw_id, NVS, muestras, unión de mesh y confirmaciones de AURA.
7. Tests sin placa: make -C dispositivos/E1-PB-LECA-HFR01/tests (C++17).
   Parser JSON real, opcional: target json con ARDUINOJSON_INCLUDE apuntando a
   ArduinoJson/src. El CI de host estándar no requiere bibliotecas Arduino.
8. Conector: python -m unittest discover -s conectores/E1-PB-LECA-HFR01/tests -v
   desde la raíz del repo, Python 3.11 o posterior, sin instalar paquetes.

## Notas

Capacidad propia: 30 registros ordinarios por sonda + primera captura protegida
por sonda + inicio del corte local. Un mensaje en vuelo tiene una copia adicional
NVS y otra en la cola oficial de LittleFS. Se conserva su JSON completo para que
un reinicio entre confirmación y limpieza no cambie los valores del mismo UUID.
La cola oficial tiene 1440 ranuras según la base, pero este adaptador entrega un
mensaje por vez: no convierte la capacidad de nuestro journal en 1440 lecturas.

Las primeras lecturas del corte llevan su hora real en ts. Si ambas sondas fallan,
se conserva localmente journal.cutAt/firstPower y se emite alerta energía, sin
telemetría falsa. El raíz de la base fecha esa alerta al publicarla; exportar la
hora original estructurada a AURA necesita acordarse con la cátedra.

Sin confirmación: envío inicial + 3 reintentos, con esperas de la base (15, 30,
60 y 120 s antes del paso a recuperación). Luego un intento cada r_s. Si la mesh
rechaza un envío, el presupuesto también se cuenta. La radio sigue escuchando
comandos y la mesh gestiona su asociación automáticamente. LED 2/OLED indican
mesh desconectada o espera de AURA agotada. No hay un ACK intermedio del raíz:
mesh conectada no demuestra que el raíz ni el broker estén funcionando.

Se conservan los namespaces anteriores y se añade iciv_aura4 para el mensaje en
vuelo; no se borra NVS ni se reemplaza la tabla de particiones. La base puede
inicializar LittleFS si no puede montarlo: respaldarlo antes de flashear.
Las alertas de la biblioteca usan RAM y no tienen ACK durable; la protección de
temperaturas y del corte local no implica entrega garantizada de esas alertas.

La mesh y el runtime central están pendientes de banco/despliegue según el
contrato recibido. Confirmar alta hw_id, datos de mesh, circuito de alimentación,
RTC y funcionamiento central. El adaptador usa structs internos expuestos por
nodo_mesh.h: revisar compatibilidad cuando cambie la biblioteca y proponer hooks
públicos en un PR separado, sin modificar comun/ en el PR de este dispositivo.

El procesamiento de las mediciones se documenta en el [README del conector](../../conectores/E1-PB-LECA-HFR01/README.md).
No versionar config_local.h, respaldos, claves ni ejecutables de tests.
