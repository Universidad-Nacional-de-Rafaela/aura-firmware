# Mesh_Intermedio_Pantalla — concentrador de aula con pantalla táctil

Cumple el mismo rol que [`Mesh3_Intermedio`](../Mesh3_Intermedio/) (concentrador de la capa 2 de la mesh, sin WiFi ni MQTT), con una pantalla táctil de 7" que muestra el estado de las hojas y maneja relés locales. Hardware: Waveshare ESP32-S3-Touch-LCD-7 (flash 8 MB, PSRAM 8 MB). Desarrollado en **ESP-IDF v6.2**.

Grupo: @maxirios, @agustin-arg.

---

### 1. Inicializar el Entorno de ESP-IDF

En cada nueva terminal de trabajo, antes de ejecutar cualquier comando de `idf.py`, carga las variables de entorno de ESP-IDF:

```bash
. $HOME/.espressif/v6.1/esp-idf/export.sh
```

> *(o alternativamente: `source $HOME/esp/esp-idf/export.sh` según tu ruta de instalación)*

---

### 2. Seleccionar el Microcontrolador Destino (Target)

Configura el microcontrolador objetivo del proyecto (**ESP32-S3**):

```bash
idf.py set-target esp32s3
```

---

### 3. Configuración de Parámetros de Malla

El centralizador se conecta a la malla mediante credenciales compartidas con el Gateway y las Hojas:

- **ID de la Malla (MESH_ID):** 6 bytes hexadecimales que identifican la red local.
- **Contraseña del AP Mesh (MESH_AP_PASS):** Clave WPA2 utilizada para la comunicación inter-nodo.
- **Aula asignada:** Identificador de aula para filtrar hojas (ej. `aula1`).

Copiar `main/config_local.ejemplo.h` como `main/config_local.h` (no se versiona) y completarlo con los mismos valores que el gateway y las hojas. Estos parámetros se configuran de forma segura mediante `main/config_local.h` o interactivamente:

```bash
idf.py menuconfig
```

Navega hasta `AURA Centralizador - Interruptores / Reles Locales --->` para configurar la cantidad de interruptores, los pines GPIO asociados y sus etiquetas descriptivas.

---

### 4. Compilación

Compila el firmware del proyecto:

```bash
idf.py build
```

---

### 5. Flasheo y Monitoreo Serie

Conecta el microcontrolador a tu computadora:

> La primera vez, o si cambió la tabla de particiones, borrar la flash antes: `idf.py -p /dev/ttyACM0 erase-flash`.

#### Flashear el firmware:

```bash
idf.py -p /dev/ttyACM0 flash
```

#### Abrir el monitor serie:

```bash
idf.py -p /dev/ttyACM0 monitor
```

#### Flashear y monitorear en un solo paso:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

> **Tip**: Para salir del monitor serie, presiona `Ctrl + ]`.

---

**OTA por la mesh:** las particiones (`ota_0`/`ota_1`), el rollback y la verificación de firma están igual que en `Mesh3_Intermedio`. El receptor `aura_ota` viene en un PR siguiente; hasta entonces se actualiza por USB.
