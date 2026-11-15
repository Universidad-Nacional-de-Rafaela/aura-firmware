# Guía del banco de pruebas: gateway, concentrador y tablero propios

> **Borrador para revisión de la cátedra.** No es una versión final. Las decisiones de planteo están al final, en «Decisiones para revisar», para que se puedan comentar una por una.

Última actualización: 15/11/2026.

## Para la IA que acompaña a un grupo

Esta guía es el paso 0 de `GUIA_HOJAS_NUEVAS.md` (en el PR #67): el grupo arma su banco antes de desarrollar y probar su hoja. Valen las mismas reglas de esa guía (orden, evidencia, credenciales y claves, repo como fuente de verdad), y dos más:

1. **Las placas del banco nunca van a producción.** Están firmadas con la clave de prueba del grupo y llevan sus datos de mesh. Una placa para la instalación la compila, la firma y la graba siempre la cátedra.
2. **En el banco no se usan datos de producción:** ni las credenciales de la red final, ni el broker de AURA.

## Para qué sirve

Cada grupo arma su propio banco: un gateway y un concentrador con placas del laboratorio, su hoja, y `broker.py` con el tablero en su notebook, todo sobre la red `AURA-BANCO`. Con eso prueba su hoja de punta a punta antes del PR. No es producción: para eso está el paso 7 de `GUIA_HOJAS_NUEVAS.md`.

## Qué se necesita

| Rol | Placa del laboratorio | Cómo reconocerla |
| --- | --- | --- |
| Gateway | ESP32-S3 con módulo **WROOM-2** y flash **octal de 32 MB** (Waveshare ESP32-S3-DEV-KIT, WROOM-2) | El módulo lleva impreso un modelo `N32R…`. `python -m esptool -p COMx flash-id` informa 32 MB, y el monitor muestra `Octal Flash Mode Enabled` |
| Concentrador | ESP32-S3 con módulo **WROOM-1** y flash de 16 MB (Waveshare ESP32-S3-DEV-KIT, WROOM-1) | El módulo lleva impreso un modelo `N16R…`, y `flash-id` informa 16 MB |
| Hoja | La del dispositivo del grupo | — |

Además: una notebook con ESP-IDF 6.1 y Python 3, la clave de `AURA-BANCO` (la pasa la cátedra en persona) y el fork del repo actualizado con `main`.

El gateway no arranca en una placa de 16 MB: su `sdkconfig.defaults` fija la flash octal de 32 MB. Adaptarlo sería modificar código de la cátedra; si no hay placas WROOM-2 disponibles, hay que consultarlo.

## Paso 1: la identidad del banco

1. **Datos de mesh propios, generados al azar.** Cada grupo tiene los suyos, para que sus placas no se mezclen con las de otros bancos:

    ```
    python -c "import secrets; print(', '.join(f'0x{b:02X}' for b in secrets.token_bytes(6))); print(secrets.token_urlsafe(18))"
    ```

    La primera línea es el `MESH_ID_BYTES`, y la segunda, la `MESH_AP_PASS`. Van en un `config_local.h` que se arma una vez y **se copia igual** a las tres placas: tipearlo tres veces es la forma más fácil de equivocarse.
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
    { "AURA-BANCO", "<clave del WiFi>" }
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

En el monitor tienen que aparecer la conexión a `AURA-BANCO` y la hora de internet. El broker lo encuentra cuando esté corriendo `broker.py` (paso 4).

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

1. **Una sola vez, como administrador:** permitir `python.exe` en las redes privadas del firewall.
2. **Que no haya un Mosquitto instalado como servicio:** `Get-NetTCPConnection -LocalPort 1883 -State Listen` no tiene que mostrar nada.
3. **Con `broker.py` apagado,** `python buscar_brokers.py`: no tiene que contestar ninguna otra computadora. Si contesta otra, el gateway se podría conectar a ese broker.
4. **`python broker.py`, sin `--usuario` ni `--clave`** (a un broker descubierto, el gateway nunca le manda credenciales), y abrir **http://localhost:8080**.

El gateway lo encuentra solo. En su monitor, la dirección del broker tiene que ser la IP de la notebook (`ipconfig`).

## Paso 5: autorizar el concentrador y asignar las aulas

1. En el tablero, **«Autorizar concentrador»**. El texto «autorizado» puede tardar hasta un minuto en actualizarse.
2. **Asignarle un aula al concentrador**, en minúsculas (solo `a-z`, `0-9`, `-` y `_`), por ejemplo `aula1`.
3. **Grabar la hoja** con la clave de prueba y el mismo `config_local.h` (`GUIA_HOJAS_NUEVAS.md`, paso 4), y asignarle la misma aula.

## Paso 6: las pruebas del banco

Primero, estas cuatro, que validan el banco antes de probar la hoja:

| N.º | Prueba | Cómo | Resultado esperado |
| --- | --- | --- | --- |
| B1 | Gateway | Encenderlo con `broker.py` corriendo | En su monitor: `AURA-BANCO`, la hora de internet y `MQTT conectado`, con la IP de la notebook. En el tablero, el gateway en línea |
| B2 | Concentrador sin autorizar | Antes de autorizarlo, encender la hoja con el aula del concentrador | El gateway descarta los lotes (`LOTE de … que no es un concentrador autorizado: se descarta`), y la hoja acumula mediciones pendientes |
| B3 | Autorizar | «Autorizar concentrador» en el tablero | `aplicado`, el gateway acepta los lotes y llegan las mediciones pendientes. La hoja puede tardar hasta unos 7,5 min en reintentar; reiniciarla acorta la espera |
| B4 | Hoja sin aula | Encender la hoja sin aula asignada | Se cuelga directo del gateway (`envío directo`), y sus mediciones se confirman |

Después, las 9 pruebas de `GUIA_HOJAS_NUEVAS.md`, paso 5. El informe de pruebas de la hoja incluye también estas cuatro.

## Paso 7: al terminar

- **No dejar el banco encendido** cuando no se usa.
- **Al devolver las placas al laboratorio,** `idf.py -p COMx erase-flash` del gateway y del concentrador, y sacarles la etiqueta. Así el siguiente grupo arranca limpio, y no quedan en las placas ni los datos de mesh ni una imagen firmada con una clave de prueba.
- **Siguiente:** `GUIA_HOJAS_NUEVAS.md`, pasos 6 y 7 (el conector, el PR y la habilitación).

## Errores comunes

| Síntoma | Causa | Solución |
| --- | --- | --- |
| El gateway no arranca, con errores de flash al iniciar | La placa no tiene flash octal de 32 MB | Usar una placa WROOM-2 de 32 MB |
| El gateway descarta los lotes del concentrador | El concentrador no está autorizado | «Autorizar concentrador» en el tablero |
| Una placa no entra a la malla | `config_local.h` con los valores de la plantilla, o distinto del de las otras placas | Copiar el mismo archivo en las tres, y recompilar |
| Aparecen en el tablero placas que no son del grupo | Placas de otro banco con los mismos datos de mesh | Generar datos de mesh propios (paso 1) |
| El gateway se conectó al broker de otra computadora | Otro `broker.py` contestó el broadcast | `buscar_brokers.py` con `broker.py` apagado; apagar el otro broker, o fijar `BROKER_URI` con la IP de la notebook |
| Al cambiar de red, el gateway tarda unos 30 s en encontrar el broker | Guarda el último broker según la posición de la red en `REDES_WIFI`, y no según su nombre (error conocido) | Se recupera solo; con `erase-flash` arranca sin el broker viejo |
| El tablero muestra placas viejas como offline, o el concentrador arranca con un aula vieja | Se grabó sin `erase-flash`, o `broker.py` conserva estados anteriores | `erase-flash` antes de grabar, y reiniciar `broker.py` |
| `ota.py` o `mandar_comando.py` se quedan esperando sin error | Un Mosquitto instalado como servicio ocupa el puerto 1883 | `--broker <IP de la notebook>`, o detener el servicio |
| La hoja está viva pero aparece `offline`, con los pendientes subiendo | Espera hasta unos 7,5 min para reintentar después de un corte | Esperar, o reiniciarla |

## Decisiones para revisar

1. **Cada grupo arma su propio gateway y concentrador, con placas del laboratorio.** Por qué: unas placas compartidas no se pueden identificar ni guardar por separado, y así cada grupo es dueño de su banco de punta a punta. Alternativa descartada: un banco compartido de la cátedra, en el que cada grupo solo graba su hoja.
2. **El gateway y el concentrador se compilan desde `main` sin tocar el código;** solo se completan `credenciales.h` y `config_local.h`. Por qué: ese código es el mismo para todos los grupos, que solo desarrollan su hoja. Un cambio ahí va en un issue o en un PR a la cátedra.
3. **Cada grupo genera sus propios datos de mesh, al azar.** Por qué: unas placas que quedan encendidas con los mismos datos se meten en mallas ajenas. Nos pasó: la cerradura y el sensor de CO₂ de otros grupos entraron a nuestra malla. Alternativa descartada: una red de pruebas común de la cátedra, más simple, pero que mezcla las placas de todos.
4. **Cada grupo usa su propia clave de prueba, y las placas del banco se etiquetan.** Por qué: la separación entre banco y producción también tiene que ser física.
5. **Se usa solo `AURA-BANCO`.** Alternativa considerada y descartada: el punto de acceso del celular, con un nombre y una clave comunes para todos los grupos. Funciona, pero suma configuración y casos especiales (la banda de 2,4 GHz y los celulares que aíslan a sus clientes).
6. **El gateway encuentra el broker por descubrimiento (`DESCUBRIR_BROKER 1`), verificando antes con `buscar_brokers.py`.** Por qué: las pruebas de los grupos no son simultáneas, y la IP de la notebook puede cambiar. Es una propuesta: la alternativa es fijar `BROKER_URI` con la IP de la notebook, que evita conectarse a otro broker, pero obliga a recompilar el gateway si la IP cambia.
7. **En el banco, `OTA_PERMITIR_HTTP` va en 1, y HTTPS es opcional.** Por qué: las pruebas obligatorias de la hoja no dependen de HTTPS, que es responsabilidad del gateway y del servidor. Quien quiera probarlo tiene el procedimiento en el informe de HTTPS de E1-PB-LECA-TAB01.
8. **`erase-flash` al armar el banco y al devolver las placas.** Por qué: la memoria de la placa conserva el aula, las autorizaciones, los nodos conocidos y el último broker. Al devolverlas borradas, el siguiente grupo arranca limpio, y no quedan datos de mesh ni una imagen firmada con una clave de prueba.
9. **Las pruebas son las 9 de la guía de hojas, más 4 del banco (B1 a B4).** Por qué: las del banco validan la infraestructura antes de culpar a la hoja de un problema que no es suyo.
10. **Va en un PR aparte del #67, con las dos guías enlazadas como un par:** esta es el paso 0 de la guía de hojas. Por qué: cada guía y cada revisión quedan más cortas.
11. **Propuesta fuera de alcance:** que cada placa informe en su estado con qué clave está firmada (por ejemplo, los primeros bytes del resumen de la clave). Así AURA marcaría en el acto una placa de producción firmada con una clave que no es la de la cátedra. Es un cambio en la biblioteca, para un PR aparte.
12. **Propuesta fuera de alcance:** que el CI del repo avise si un PR sube un `.bin` o algo con forma de clave: `gitleaks` no ve un `.bin` firmado como un secreto, pero lleva las credenciales compiladas.
