# ADR-008 — Actualización por la malla (OTA) y un tipo de firmware por uso

**Estado:** propuesta, probada en placa · **Fecha:** 2026-10-22 · **Decide:** Matías Wanzenried, a partir del trabajo de Milton Pozzo
**Sigue a:** [ADR-007](ADR-007-mesh-en-esp-idf-con-concentrador-por-aula.md) (mesh en ESP-IDF con concentrador por aula).
**Afecta a:** `docs/CONTRATO_MQTT.md` (v4.3, propuesta: comando `ota` y `details.firmware`), el
repositorio público `aura-firmware` (`Mesh_Red_Completa/componentes/aura_ota`, tablas de particiones
de los tres proyectos) y **todos los dispositivos de interior** de los grupos (`dispositivos/<CÓDIGO>/`):
su `CMakeLists.txt` declara el tipo y la versión, y su `partitions.csv` pasa a tener dos lugares para
el firmware.

## El problema

Hoy cada placa de la mesh se actualiza **por USB**, una por una. En el edificio van a estar en
techos, tableros y paredes, y un mismo uso (por ejemplo, todos los controles de acceso) se repite en
muchas aulas: corregir un error en ese firmware es recorrer el edificio con una notebook.

Hace falta poder mandar un firmware nuevo **desde AURA**, a una placa o a todas las de un uso, sin
que eso pueda:

- instalarle a una placa el firmware de **otro uso** (el de un sensor de temperatura en un control
  de acceso);
- dejar una placa **muerta** si la versión nueva no anda;
- dejar un aula **sin todos sus dispositivos a la vez**, ni saturar la malla.

Y para elegir "todas las de un uso" hace falta que cada placa **diga qué firmware tiene**.

## El tipo de firmware

**Opciones.** A) el nombre del proyecto (`project()` del `CMakeLists.txt`); B) una constante en el
código (`#define TIPO "acceso"`); C) un valor en NVS que asigne AURA.

**Decisión: A.** ESP-IDF ya graba en cada binario el nombre del proyecto y su versión
(`esp_app_desc_t`, sin código extra). Se leen del `.bin` **antes** de grabarlo (el gateway y
`ota.py` lo hacen) y la placa los lee con `esp_app_get_description()`. No pueden quedar distintos de
la imagen, como podría pasar con B o C.

**Regla** (`GUIA_NODOS_HOJA.md`, sección "Tipo y versión del firmware"):

| | Regla | Ejemplos |
|---|---|---|
| **Tipo** | `project(aura-<uso>)`: `aura-` y después `a-z`, `0-9` y `-`, hasta 31 caracteres en total. **Todas las placas de un mismo uso llevan el mismo tipo y el mismo binario.** | `aura-acceso`, `aura-temp-heladera`, `aura-iluminacion`, `aura-concentrador`, `aura-gateway`, `aura-hoja-ejemplo` |
| **Versión** | `set(PROJECT_VER "mayor.menor.parche")` antes de `project()`. Se sube en **cada** entrega. | `1.0.0`, `1.3.2` |

El tipo identifica **el firmware**; el código de dispositivo (`dispositivos/E1-AZ-TERR-TAN01`)
identifica **una instalación**. Conviven: muchas instalaciones, un mismo tipo. Si dos grupos hacen
dispositivos distintos, son dos tipos distintos aunque usen la misma placa.

Cada placa informa su tipo y versión en el reporte; el raíz lo publica en
`hw/<hw_id>/status` como `details.firmware: {"tipo": "aura-acceso", "version": "1.3.0"}` (contrato
§3.2, v4.3). Con eso AURA sabe qué placas son de cada tipo, sin una tabla aparte.

## La distribución

**Opciones.** A) que cada placa descargue la imagen por HTTP: las hojas no tienen IP (ADR-006), no se
puede. B) que el gateway la descargue una vez y la reparta por la malla. C) la OTA de malla de
Espressif (ESP-MDF `mupgrade`): no está en ESP-IDF 6 y el proyecto ESP-MDF está discontinuado.

**Decisión: B**, con estas reglas (implementadas en `componentes/aura_ota` y
`Mesh3_Gateway/main/ota_gateway.c`):

1. **AURA pide la actualización por placa**: `hw/<hw_id>/command` con
   `{"command":"ota","params":{"url":"…","sha256":"…"}}` (contrato §3.3, v4.3). "Todas las de un
   tipo" es AURA (o `ota.py` en el banco) mandando ese comando a cada `hw_id` cuyo
   `details.firmware.tipo` sea el de la imagen, con filtro opcional por aula.
2. **El gateway descarga la imagen una vez** (HTTPS; HTTP solo en el banco) a su partición `imagen`, verifica el
   SHA-256 contra el del comando y lee tipo, versión y chip de la imagen.
3. **Una placa por vez.** El gateway la ofrece por la malla y el nodo la pide en ventanas de
   4 × 1400 B (un nodo que pierde un bloque lo vuelve a pedir). La malla no se satura y un aula nunca
   se queda sin todos sus dispositivos a la vez. 1,1 MB tarda ~10 s en pasar.
4. **Controles antes de instalar**, en el gateway y otra vez en el nodo:
   - mismo chip;
   - mismo tipo, sin excepciones: un cambio de uso se hace por USB (`forzar_tipo` se eliminó en la
     revisión de seguridad, 2026-10-26);
   - **firma de la cátedra** (RSA-3072, sin eFuses): la placa rechaza una imagen sin firmar o
     firmada con otra clave, y acepta las tramas de la actualización solo del raíz;
   - otra versión: si es la misma, contesta `aplicado` con `sin_cambios`;
   - que la imagen diga lo mismo que la oferta;
   - SHA-256 de lo recibido;
   - imagen válida de ESP-IDF.
5. **El nodo escribe en su otra partición**, sin dejar de medir ni de responder, y se reinicia con la
   versión nueva. Antes respalda en flash su cola y espera a que salgan sus eventos: no se pierde
   nada.
6. **Vuelta atrás:** la versión nueva arranca "a prueba". Queda solo si **habla con el gateway** (el
   gateway la ve volver con la versión nueva) **y anda 60 s sin colgarse**. Si se reinicia antes, o
   si en 10 min no lo logra, el bootloader vuelve solo a la versión anterior y AURA recibe `rechazado`
   con `volvio_a_la_version_anterior`. `aplicado` llega recién cuando la versión quedó.
7. **El gateway se actualiza último** (después de 15 s sin otros pedidos en la cola) y vuelve atrás
   si la versión nueva no llega al broker.

## Qué se gana y qué se pierde

**Se gana:**

- corregir un firmware en todo el edificio desde AURA, en ~1 min por placa;
- imposible instalar el firmware de otro uso por error, y difícil dejar una placa muerta (probado:
  una versión que se cuelga a los 20 s vuelve sola a la anterior);
- AURA sabe qué firmware tiene cada placa.

**Se pierde:**

- **la primera vez, todas las placas se graban por USB** (cambia la tabla de particiones);
- el lugar para el firmware de una hoja de 4 MB baja a **1,75 MB** (dos lugares en lugar de uno);
  la hoja de ejemplo ocupa 1,05 MB;
- un reinicio en los primeros 60 s de una versión nueva (un corte de luz, por ejemplo) la descarta y
  vuelve a la anterior: hay que volver a pedirla;
- AURA tiene que **servir la imagen** por HTTP(S) e implementar el comando, el seguimiento de las
  respuestas y el reenvío de lo que no terminó si el gateway se reinicia.

## Lo que esta decisión NO resuelve

- **Secure Boot.** Desde el 2026-10-26 las imágenes van firmadas y cada placa verifica la firma al
  instalarlas, pero no en cada arranque: eso es Secure Boot V2, que quema eFuses y queda para las
  placas de la instalación final. Quién custodia la clave de firma es el pendiente 15 del contrato.
- **Volver a una versión más vieja** está permitido (útil para deshacer una entrega). Si hiciera
  falta impedirlo: anti-rollback por eFuse.
- La actualización de los dispositivos **LoRaWAN** (FUOTA) es otro mecanismo.

## Disparador de revisión

- Si con muchas hojas por aula (más de 10) la actualización de un aula tarda más de lo aceptable:
  repartir a varias placas a la vez (multicast).
- Si una imagen deja de entrar en 1,75 MB: placas de 8 MB para ese tipo.
