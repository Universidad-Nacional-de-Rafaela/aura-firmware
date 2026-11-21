# Guía del banco de pruebas: gateway, concentrador y tablero propios

## Para la IA que acompaña a un grupo

Esta guía es el paso 0 de [`GUIA_HOJAS_NUEVAS.md`](GUIA_HOJAS_NUEVAS.md): el grupo arma y valida su banco antes de desarrollar y probar su hoja. Valen las mismas reglas de esa guía (orden, evidencia, credenciales y claves, repo como fuente de verdad), y dos más:

1. **Las placas del banco nunca van a producción.** Están firmadas con la clave de prueba del grupo y llevan sus datos de mesh. Una placa para la instalación la compila, la firma y la graba siempre la cátedra.
2. **En el banco no se usan datos ni redes de producción:** ni las credenciales de la red final, ni el broker de AURA. Y el banco nunca se conecta a la red de producción, aunque esté al alcance.

## Para qué sirve

Cada grupo arma su propio banco: un gateway y un concentrador con placas que presta la cátedra, y `broker.py` con el tablero en su notebook, todo sobre la red del banco, que no es la de producción. El banco se valida una vez con la hoja de ejemplo de la cátedra (`Mesh3_Hoja`), así no depende de la hoja del grupo, que se arma recién en los pasos 1 a 4 de `GUIA_HOJAS_NUEVAS.md`. No es producción: para eso está el paso 7 de esa guía.

## Qué se necesita

| Rol | Placa | Cómo reconocerla |
| --- | --- | --- |
| Gateway | ESP32-S3 con módulo **WROOM-2** y flash **octal de 32 MB** (Waveshare ESP32-S3-DEV-KIT, WROOM-2) | El módulo lleva impreso un modelo `N32R…`. `python -m esptool -p COMx flash-id` informa 32 MB, y el monitor muestra `Octal Flash Mode Enabled` |
| Concentrador | ESP32-S3 con módulo **WROOM-1** y flash de 16 MB (Waveshare ESP32-S3-DEV-KIT, WROOM-1) | El módulo lleva impreso un modelo `N16R…`, y `flash-id` informa 16 MB |
| Hoja de ejemplo | ESP32-C3 SuperMini, con `Mesh3_Hoja` tal cual: manda datos simulados, así que no necesita sensores | — |

Además: una notebook con ESP-IDF 6.1 y Python 3, el fork del repo actualizado con `main`, y **la red del banco (2,4 GHz, con internet y sin aislamiento de clientes)**. El nombre y la clave los pasa la cátedra en persona.

- **Con internet,** porque el gateway saca la hora por SNTP (`pool.ntp.org`), y sin hora las mediciones no llevan `ts`, que las pruebas de la hoja exigen.
- **De 2,4 GHz y sin aislamiento de clientes,** porque el gateway encuentra a `broker.py` por broadcast en esa red, y después se conecta a la notebook.

El gateway no arranca en una placa sin flash octal: su `sdkconfig.defaults` fija la flash octal de 32 MB, y el arranque se frena con `Octal Flash option selected, but EFUSE not configured!`. Adaptarlo sería modificar código de la cátedra; si no hay placas WROOM-2 disponibles, hay que consultarlo.

## Paso 1: la identidad del banco

1. **Datos de mesh propios, generados al azar.** Cada grupo tiene los suyos, para que sus placas no se mezclen con las de otros bancos:

    ```
    python -c "import secrets; print(', '.join(f'0x{b:02X}' for b in secrets.token_bytes(6))); print(secrets.token_urlsafe(18))"
    ```

    Van en un `config_local.h` que se arma una vez, copiando la plantilla `main\config_local.ejemplo.h` de cualquiera de los proyectos. La primera línea del comando es el `MESH_ID_BYTES`, y la segunda, la `MESH_AP_PASS`, que va entre comillas:

    ```
    #define MESH_ID_BYTES   <primera línea>
    #define MESH_AP_PASS    "<segunda línea>"
    ```

    Ese mismo archivo **se copia igual** a todas las placas del banco: tipearlo en cada una es la forma más fácil de equivocarse.
2. **La clave de firma de prueba del grupo:** la misma de `GUIA_HOJAS_NUEVAS.md`, paso 3.
3. **Una etiqueta en cada placa del banco**, por ejemplo «BANCO – grupo X – no instalar».

Ni el `config_local.h`, ni el `credenciales.h`, ni la clave se suben al repo (`GUIA_HOJAS_NUEVAS.md`, paso 3).

## Paso 2: el gateway

Desde la terminal de ESP-IDF (PowerShell), en `Mesh_Red_Completa\Mesh3_Gateway`:

```
copy main\credenciales.ejemplo.h main\credenciales.h
copy <el config_local.h del paso 1> main\config_local.h
```

En `main\credenciales.h`, modificar las líneas que ya están (no agregar otras, porque quedarían definidas dos veces):

```
#define REDES_WIFI \
    { "<red del banco>", "<clave del WiFi>" }
#define BROKER_URI      ""
#define DESCUBRIR_BROKER 1
#define MQTT_USUARIO    ""
#define MQTT_CLAVE      ""
#define OTA_PERMITIR_HTTP 1
```

`REDES_WIFI` queda con una sola red, sin la línea `OTRA_RED` ni la barra invertida al final. `OTA_CA_PEM` queda comentado: probar HTTPS es opcional, y el procedimiento está en `dispositivos/E1-PB-LECA-TAB01/pruebas/2026-11-06-ota-https/`. Después:

```
idf.py set-target esp32s3
idf.py build
python ..\Broker_MQTT\firmar.py --clave C:\aura-claves\firma_prueba.pem build\aura-gateway.bin
idf.py -p COMx erase-flash
idf.py -p COMx flash monitor
```

En el monitor tienen que aparecer la conexión a la red del banco y la hora de internet. Hasta que corra `broker.py` (paso 4), es normal ver `No encuentro el broker`.

## Paso 3: el concentrador

En `Mesh_Red_Completa\Mesh3_Intermedio`, solo con el `config_local.h` del paso 1:

```
copy <el config_local.h del paso 1> main\config_local.h
idf.py set-target esp32s3
idf.py build
python ..\Broker_MQTT\firmar.py --clave C:\aura-claves\firma_prueba.pem build\aura-concentrador.bin
idf.py -p COMx erase-flash
idf.py -p COMx flash
```

En el monitor del gateway tienen que aparecer `Hijo conectado` y `total en la malla: 2 nodos`.

## Paso 4: el broker y el tablero

En `Mesh_Red_Completa\Broker_MQTT`:

1. **Una sola vez, como administrador:** permitir `python.exe` en las redes privadas del firewall. Además, Windows tiene que tener **la red del banco marcada como Privada**: si está como Pública, esa regla no se aplica (ver `Broker_MQTT/LEEME.md`).
2. **Que no haya un Mosquitto instalado como servicio:** `Get-NetTCPConnection -LocalPort 1883 -State Listen` no tiene que mostrar nada.
3. **Con `broker.py` apagado,** `python buscar_brokers.py`: no tiene que contestar ninguna otra computadora. Si contesta otra, el gateway se podría conectar a ese broker.
4. **`python broker.py`, sin `--usuario` ni `--clave`** (a un broker descubierto, el gateway nunca le manda credenciales), y abrir **http://localhost:8080**.

## Paso 5: validar el banco con la hoja de ejemplo

El banco se valida una vez, con la hoja de ejemplo de la cátedra tal cual, en el orden en que pasan las cosas. Estas cuatro pruebas validan el banco: no van en el informe de cada hoja, que lleva las 9 pruebas de `GUIA_HOJAS_NUEVAS.md`.

1. **B1, el gateway con el broker.** Con `broker.py` corriendo, el gateway lo encuentra solo. En su monitor aparecen `Broker: mqtt://<IP>:1883`, con la IP de la notebook (`ipconfig`), y después `MQTT conectado como mac-…`. En el tablero, el gateway queda en línea.
2. **B2, la hoja sin aula.** En `Mesh_Red_Completa\Mesh3_Hoja`, sin tocar el código, grabar la hoja de ejemplo con el `config_local.h` del paso 1 y la clave de prueba:

    ```
    copy <el config_local.h del paso 1> main\config_local.h
    idf.py set-target esp32c3
    idf.py build
    python ..\Broker_MQTT\firmar.py --clave C:\aura-claves\firma_prueba.pem build\aura-hoja-ejemplo.bin
    idf.py -p COMx erase-flash
    idf.py -p COMx flash monitor
    ```

    Arranca sin aula y se cuelga directo del gateway (`envío directo`), y sus mediciones se confirman (`persistido`).
3. **B3, el concentrador sin autorizar.** En el tablero, asignarles la misma aula al concentrador y a la hoja, sin autorizar el concentrador. El aula va en minúsculas, solo con `a-z`, `0-9`, `-` y `_`, y hasta 15 caracteres; por ejemplo, `aula1`. La hoja pasa al concentrador, el gateway descarta sus lotes (`LOTE de … que no es un concentrador autorizado: se descarta`), y la hoja acumula mediciones pendientes.
4. **B4, autorizar.** En el tablero, «Autorizar concentrador»: `aplicado`, el gateway acepta los lotes y llegan las mediciones pendientes. La hoja puede tardar hasta unos 7,5 min en reintentar, y reiniciarla acorta la espera. El texto «autorizado» del tablero puede tardar hasta un minuto en actualizarse.

## Paso 6: la hoja del grupo

Con el banco validado, volvé a [`GUIA_HOJAS_NUEVAS.md`](GUIA_HOJAS_NUEVAS.md), paso 1. La hoja de ejemplo ya no hace falta: las 9 pruebas se hacen con la hoja del grupo, en este mismo banco.

## Cuando ya no se use el banco

- **No dejar el banco encendido** cuando no se usa.
- **Al devolver las placas,** `idf.py -p COMx erase-flash` del gateway, del concentrador y de la hoja de ejemplo, y sacarles la etiqueta. Así el siguiente grupo arranca limpio, y no quedan en las placas ni los datos de mesh ni una imagen firmada con una clave de prueba.

## Errores comunes

| Síntoma | Causa | Solución |
| --- | --- | --- |
| El gateway no arranca: `Octal Flash option selected, but EFUSE not configured!` | La placa no tiene flash octal | Usar una placa WROOM-2 de 32 MB |
| El gateway muestra `No encuentro el broker` | `broker.py` no está corriendo, la red está marcada como Pública, o el firewall bloquea a `python.exe` | Arrancar `broker.py`, marcar la red como Privada y revisar el firewall (paso 4) |
| Las mediciones llegan sin `ts` | La red del banco no tiene internet, y el gateway no consigue la hora | Usar una red con internet |
| El gateway descarta los lotes del concentrador | El concentrador no está autorizado | «Autorizar concentrador» en el tablero |
| El aula queda `rechazado` (`aula_invalida`) | Tiene mayúsculas, otros caracteres, o más de 15 | Solo `a-z`, `0-9`, `-` y `_`, hasta 15 caracteres |
| Una placa no entra a la malla | `config_local.h` con los valores de la plantilla, o distinto del de las otras placas | Copiar el mismo archivo en todas, y recompilar |
| Aparecen en el tablero placas que no son del grupo | Placas de otro banco con los mismos datos de mesh | Generar datos de mesh propios (paso 1) |
| El gateway se conectó al broker de otra computadora | Otro `broker.py` contestó el broadcast | `buscar_brokers.py` con `broker.py` apagado; apagar el otro broker, o fijar `BROKER_URI` con la IP de la notebook |
| El tablero muestra placas viejas como offline, o el concentrador arranca con un aula vieja | Se grabó sin `erase-flash`, o `broker.py` conserva estados anteriores | `erase-flash` antes de grabar, y reiniciar `broker.py` |
| `ota.py` o `mandar_comando.py` se quedan esperando sin error | Un Mosquitto instalado como servicio ocupa el puerto 1883 | `--broker <IP de la notebook>`, o detener el servicio |
| La hoja está viva pero aparece `offline`, con los pendientes subiendo | Espera hasta unos 7,5 min para reintentar después de un corte | Esperar, o reiniciarla |
