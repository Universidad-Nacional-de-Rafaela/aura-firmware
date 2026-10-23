# Mesh_Red_Completa – la mesh de AURA en ESP-IDF (contrato MQTT v4.0)

Red de un aula **completa y probada en placas**, de punta a punta: hoja → concentrador del aula → gateway (raíz) →
MQTT (`hw/<hw_id>/...`) y los comandos en sentido inverso. Hecha en **ESP-IDF 6.1** (C, sin Arduino) y cumple
[`docs/CONTRATO_MQTT.md`](../docs/CONTRATO_MQTT.md) v4.0.

> **⚠️ Desde el 2026-10-20, esta es la base de la mesh de AURA.** Todos los nodos de la mesh (raíz, concentradores y
> las hojas de los grupos) pasan a **ESP-IDF nativo** con esta estructura. La versión Arduino de `main`
> (`infraestructura/`, `comun/radio_wifi_mesh.h`, `comun/nodo_mesh.h`) **se reemplaza** por esta, así que quien trabaje
> sobre la mesh tiene que pasarse a este formato. Motivo: la capa `WiFi.h` de Arduino se pelea con `esp_mesh`
> (reconexión propia, credenciales en la NVS) y no deja elegir el padre por aula.
>
> Las hojas de los grupos se programan sobre la biblioteca [`componentes/nodo_mesh`](componentes/nodo_mesh/): ver
> [`GUIA_NODOS_HOJA.md`](GUIA_NODOS_HOJA.md).

```
                        broker MQTT (aura-app, o Broker_MQTT/broker.py en el banco)
                                          ▲  hw/<hw_id>/{data,status,response,alerts,ack,command}
                                          │ WiFi 2,4 GHz (único nodo que depende del WiFi)
                                ┌─────────┴──────────┐
                                │  Mesh3_Gateway     │  raíz · ESP32-S3 · adaptador del contrato
                                └─────────┬──────────┘
                                          │ malla ESP-WIFI-MESH (trama binaria v3 + LOTE + HORA)
                                ┌─────────┴──────────┐
                                │  Mesh3_Intermedio  │  concentrador del aula · ESP32-S3
                                │  pantalla + botón  │
                                └─────────┬──────────┘
                                          │
                                ┌─────────┴──────────┐
                                │  Mesh3_Hoja        │  sensores/actuadores · ESP32-C3 (sobre nodo_mesh)
                                └────────────────────┘
```

## Qué hay en cada carpeta

| Carpeta / archivo | Qué es |
|---|---|
| [`componentes/aura_mesh/`](componentes/aura_mesh/) | Trama común de la mesh (solo encabezados): la **trama binaria v3**, igual a [`comun/protocolo_aura.h`](../comun/protocolo_aura.h), más dos tipos internos (**LOTE** y **HORA**), y utilidades (ingest_id, hw_id, tópicos, hora ISO, vendor IE). La usan los tres firmwares. |
| [`componentes/nodo_mesh/`](componentes/nodo_mesh/) | **Biblioteca de la hoja**: elección del padre por aula, aula en NVS, cola de mediciones híbrida (RAM, y flash si pasan 5 min sin confirmar) con confirmación de AURA, eventos confirmados, comandos, hora y alertas. El grupo solo escribe qué mide, sus parámetros y sus comandos. |
| [`Mesh3_Gateway/`](Mesh3_Gateway/) | **Gateway (raíz).** Único nodo con WiFi y adaptador del contrato: publica en `hw/<hw_id>/...` con el `hw_id` de la placa que midió (también lo que llega en un LOTE del concentrador), baja comandos y `ack`, infiere offline, publica su status con LWT. Lista de redes WiFi, broker guardado por red y descubrimiento por broadcast. Placa: Waveshare ESP32-S3-DEV-KIT, módulo WROOM-2 (flash octal 32 MB). |
| [`Mesh3_Intermedio/`](Mesh3_Intermedio/) | **Concentrador del aula.** Aula en NVS (la asigna AURA), anunciada en el beacon. Ve los datos de sus hojas, los muestra ("pantalla": LED RGB + serie), puede filtrarlos y los reenvía **una sola vez** en un LOTE. Contesta la hora a sus hojas y tiene **mando local** (botón BOOT). Placa: Waveshare ESP32-S3-DEV-KIT, módulo WROOM-1 (flash quad 16 MB). |
| [`Mesh3_Hoja/`](Mesh3_Hoja/) | **Hoja de ejemplo** sobre `nodo_mesh` (datos simulados + LED como actuador, parámetro `intervalo_s`). Es la **base para hacer hojas nuevas**. Placa: ESP32-C3 SuperMini (partición `cola` de 256 KB para el respaldo). |
| [`Broker_MQTT/`](Broker_MQTT/) | Banco en la PC, en Python sin dependencias: `broker.py` (broker con retenidos y LWT, **AURA falsa** que contesta los `ack`, descubrimiento y tablero en http://localhost:8080), `dashboard.html`, `led_blink.py` y [`LEEME.md`](Broker_MQTT/LEEME.md) (cómo probar y resultados). |
| [`GUIA_NODOS_HOJA.md`](GUIA_NODOS_HOJA.md) | **Guía para quienes hagan hojas:** la API de `nodo_mesh`, reglas, qué va en `values`, comandos, checklist, **parámetros de red y dónde replicar cada cambio**, problemas frecuentes y alcance. |
| [`CONTEXTO_PROYECTO.md`](CONTEXTO_PROYECTO.md) | Contexto técnico: entorno, decisiones, errores resueltos y próximos pasos (copia del `CLAUDE.md` local). |

Dentro de cada proyecto ESP-IDF:

| Archivo | Para qué |
|---|---|
| `CMakeLists.txt` | Proyecto (`project(...)`); suma `../componentes` |
| `sdkconfig.defaults` | Ajustes de la placa (target, flash, particiones). El `sdkconfig` se genera al compilar. |
| `partitions.csv` | Solo en la hoja: app de 1,5 MB + partición `cola` de 256 KB |
| `dependencies.lock` | Versiones fijas de los componentes externos (`espressif/cjson`, `espressif/mqtt`, `espressif/led_strip`) |
| `main/main.c` | El firmware, comentado |
| `main/config_local.ejemplo.h` | Plantilla del **ID y la clave de la mesh** |
| `main/credenciales.ejemplo.h` | Solo en el gateway: **redes WiFi**, broker y usuario MQTT |

## Cómo compilar y probar

**Requisitos:** ESP-IDF 6.1 y Python 3. Ruta sin espacios ni tildes, fuera de Google Drive y OneDrive. Los proyectos
tienen que quedar **al lado de `componentes/`**.

1. **Configuración local (no se sube al repo):**
   - En los tres proyectos, copiar `main/config_local.ejemplo.h` como `main/config_local.h` y completar `MESH_ID_BYTES` y
     `MESH_AP_PASS` (los da la cátedra; **iguales en los tres**).
   - En el gateway, copiar `main/credenciales.ejemplo.h` como `main/credenciales.h` y completar `REDES_WIFI` (una o más
     redes `{"SSID", "clave"}` en orden de preferencia, 2,4 GHz, sin portal de login ni aislamiento de clientes). Contra el
     broker de aura-app, completar `MQTT_USUARIO`/`MQTT_CLAVE` y `BROKER_URI`.
2. **Compilar y grabar** cada proyecto, desde su carpeta:
   ```bash
   idf.py set-target esp32s3        # esp32c3 para Mesh3_Hoja
   idf.py -p COMx build flash monitor
   ```
3. **Banco** en la PC (misma red que el gateway; en Windows, red *Privada*):
   ```bash
   python Broker_MQTT/broker.py
   ```
   Abrir http://localhost:8080. Las placas aparecen **pendientes** (sin aula): asignarles el aula con el botón del tablero
   (hoja y concentrador con la misma). Con el backend real de AURA, arrancar el broker de banco con `--sin-aura` o no usarlo.

**Las placas no tienen que tocarse:** pegadas, la raíz y el concentrador se desconectan cada ~11 s y la hoja no logra
autenticarse (ver la guía, sección 5).

## Cómo cumple el contrato

| Tema | Cómo quedó |
|---|---|
| Formato dentro de la mesh | **Trama binaria v3** (cabecera de 5 B: versión, tipo, `seq`, largo) con `values` en JSON adentro. |
| Tipos | TELEMETRÍA, COMANDO, CONFIRMACIÓN, RESULTADO, REPORTE, ALERTA, y dos internos: **LOTE** (el concentrador junta tramas de sus hojas, cada una con su MAC) y **HORA** (pedido y respuesta). |
| Sin duplicados | Cada dato viaja **una sola vez**: hoja → concentrador (lo ve y lo puede filtrar) → raíz, en LOTE. |
| Identidad | `hw_id = mac-<MAC de la placa>`. Sin MACs, ids ni aulas en los firmwares: todas las hojas de un tipo llevan el mismo binario. |
| Alta y recambio | Placa nueva → entra sin aula (directo a la raíz) → AURA la ve **pendiente** (cuarentena) → al aceptarla le asigna el dispositivo y `set_config {"aula":…}` → la hoja lo guarda en NVS y se pasa a su concentrador. |
| Aula sin red arriba | La hoja le manda al concentrador su último valor (`ESTADO`, no sube a la raíz) cuando hay cola atrás y al colgarse de él; el concentrador muestra solo lo último y su botón comanda la hoja sin gateway ni MQTT. Con 60 s sin gateway, el concentrador pasa a **modo isla** (raíz de su aula) para que las hojas puedan volver a colgarse, y vuelve solo. |
| Elección del padre | **A mano, por aula** (vendor IE del concentrador), con plan B a la raíz u otro concentrador. Si al concentrador le cambian el aula, sus hojas de otra aula se van en ~5 s. |
| Hoja y `MESH_LEAF` | IDF 6.1 rechaza `MESH_LEAF` en `esp_mesh_set_parent` (probado). La hoja intenta `MESH_LEAF` y, si no, usa `MESH_NODE` con el beacon de su AP a 60 000 TU (~200 veces menos) y rechazando hijos. |
| Entrega garantizada | **Cola híbrida**: RAM (400 mediciones, sin escribir la flash en marcha normal) y respaldo en flash de lo que pasa 5 min sin confirmar, que sobrevive a un reinicio de la hoja; `ingest_id` generado en la hoja, sale solo con la CONFIRMACIÓN que sigue al `ack` de AURA (§3.6, §5). Reintentos con espera creciente y azar. |
| Eventos | Resultado, reporte y alerta se reintentan hasta que la raíz confirma que los **publicó** (`EVENTO_OK`). Al volver el broker, la raíz pide reporte a todos (`REPORTAR`) y la hoja manda lo pendiente enseguida. |
| Hora | SNTP en la raíz; la hoja la pide y la ajusta con cada confirmación. Lo medido sin hora se fecha con el uptime si es del mismo arranque. |
| País | `AR` (canales 1 a 13) en los tres. |
| Offline y ausencias | La raíz publica `offline` si un nodo sale de la tabla de ruteo (`fuera_de_la_malla`, 20 s) o pasa 3 intervalos sin datos (`sin_tramas`), y recuerda los nodos en NVS para hacerlo también tras reiniciarse. El concentrador marca **ausente** (LED rojo) a la hoja de su aula que falta y la recuerda aunque se reinicie. |
| Raíz | Árbol `hw/`, `ack`, LWT, sesión persistente, usuario y clave, `status` con details, offline inferido, estados `transmitido`/`recibido`/`aplicado`/`rechazado`, 30 s → `sin_resultado_del_nodo`. MQTT sin bloquear la malla. |

## Qué se probó en placas (2026-10-20)

- Hoja sin aula publicando como pendiente; asignación de aula desde el tablero (`aplicado` con la config vigente) y paso
  automático al concentrador; datos en LOTE (55 lotes, 0 tramas inválidas).
- Cola: mediciones guardadas sin conexión, enviadas y confirmadas al volver, todas con `ts`. Broker caído 40 s: el reporte se reintentó, y al volver la raíz pidió reporte y las 5 pendientes salieron en el acto. Broker caído justo al cambiar el LED: AURA terminó con el estado real. Cola híbrida: 15 mediciones respaldadas en flash, recuperadas tras reiniciar la hoja y entregadas con su hora original.
- Comandos a través del concentrador (`aplicado` en 29–51 ms; uno desconocido, `rechazado`); botón local del concentrador solo sobre hojas de su aula.
- Plan B y vuelta sin pérdidas; offline inferido a los 3 intervalos; AURA caída (`sin_ack`) con reintentos 15 s/30 s/60 s y
  vaciado al volver; cambio de aula del concentrador con la hoja de otra aula pasando a plan B en ~5 s.
- Lista de redes WiFi y broker guardado (conecta en 0,1 s).
- De antes (formato viejo): una noche entera sin reinicios, cambio de red WiFi, vigilante de envío (recuperación en 10–25 s)
  y alcance de la C3 SuperMini (~10 m por una vidriera con antena de alambre de 31 mm y 15 dBm).

Pendiente: una prueba larga. Detalle con fechas en
[`Broker_MQTT/LEEME.md`](Broker_MQTT/LEEME.md).
