# Firmware mesh ESP-NOW para AURA — Plan de implementación

> **Para agentes:** SUB-SKILL REQUERIDA: usar `superpowers:subagent-driven-development` (recomendado)
> o `superpowers:executing-plans` para ejecutar tarea por tarea. Los pasos usan checkboxes (`- [ ]`).

**Goal:** Llevar la lectura de un sensor de luz desde dentro de un aula hasta el backend de AURA
atravesando dos saltos ESP-NOW, y permitir que los comandos bajen por el mismo camino.

**Architecture:** Cadena fija `nodo_sensor → nodo_sala → nodo_gateway`. Los dos primeros se
identifican por MAC y no conocen AURA; el gateway traduce MAC→UUID y es el único que habla
MQTT y HTTP. La lógica que se puede probar sin hardware (trama, buffer, ingest_id) vive en
headers de `firmware/comun/` y se testea en el host con g++ antes de tocar una placa.

**Tech Stack:** Arduino-ESP32 core 3.3.11, ESP-NOW (`esp_now.h`), `ArduinoMqttClient`,
`HTTPClient`, `ArduinoJson`, BH1750. Tests de host en C++ con g++ y aserciones propias.

**Spec:** `docs/superpowers/specs/2026-08-28-firmware-mesh-espnow-design.md`

## Global Constraints

- Versión de protocolo: `AURA_PROTO_VERSION = 1`. Toda trama con otra versión se descarta.
- Trama: 197 bytes (cabecera 17 + payload 180). Límite duro de ESP-NOW: **250 bytes**. No superarlo.
- Core Arduino-ESP32 **3.3.11**: el callback de recepción es
  `void cb(const esp_now_recv_info_t*, const uint8_t*, int)`. No registrar callback de envío
  (su firma cambió en 3.x y rompe la compilación al copiar ejemplos viejos).
- Tópicos MQTT exactos, tomados del código (`device_integration.py`), **no** de la documentación:
  `devices/{uuid}/data`, `devices/{uuid}/status`, `devices/{uuid}/command`, `devices/{uuid}/response`.
- Endpoint REST de telemetría: `POST /api/v1/telemetry/ingest`, cuerpo `{"events": [...]}`,
  cada evento con `tenant_id`, `device_id`, `type`, `payload`, `ingest_id`.
- Los tres nodos operan en el **canal del AP**. Ningún nodo fija un canal distinto.
- Nombres de archivo y comentarios en español, sin tildes en identificadores de C.

---

## Estructura de archivos

```
firmware/
├── comun/
│   ├── protocolo_aura.h       # TramaAura, validación, helpers de direccionamiento
│   ├── buffer_circular.h      # buffer FIFO con descarte del más viejo
│   └── ingest_id.h            # UUID determinístico para idempotencia
├── tests_host/
│   ├── aserciones.h           # mini framework de asserts
│   ├── test_protocolo.cpp
│   ├── test_buffer.cpp
│   ├── test_ingest_id.cpp
│   └── Makefile
├── nodo_sensor/nodo_sensor.ino
├── nodo_sala/nodo_sala.ino
└── nodo_gateway/nodo_gateway.ino
```

Los tres headers de `comun/` son C puro sin dependencias de Arduino: por eso se pueden
compilar en el host. Esa es la razón de que la lógica riesgosa viva ahí y no en los `.ino`.

---

### Task 1: Protocolo de trama + arnés de tests en host

**Files:**
- Create: `firmware/comun/protocolo_aura.h`
- Create: `firmware/tests_host/aserciones.h`
- Create: `firmware/tests_host/test_protocolo.cpp`
- Create: `firmware/tests_host/Makefile`

**Interfaces:**
- Consumes: nada.
- Produces: `TramaAura`, `AURA_TIPO_TELEMETRIA|COMANDO|ACK`, `AURA_CABECERA_BYTES`,
  `aura_trama_init()`, `aura_trama_bytes()`, `aura_trama_valida()`, `aura_es_para_mi()`.

- [ ] **Step 1: Escribir el arnés de aserciones**

`firmware/tests_host/aserciones.h`:

```cpp
#pragma once
#include <cstdio>
#include <cstring>

static int g_fallos = 0;
static int g_corridos = 0;

#define VERIFICAR(cond)                                                    \
  do {                                                                     \
    g_corridos++;                                                          \
    if (!(cond)) {                                                         \
      g_fallos++;                                                          \
      printf("  FALLO %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                      \
  } while (0)

#define RESUMEN()                                                          \
  do {                                                                     \
    printf("%d verificaciones, %d fallos\n", g_corridos, g_fallos);        \
    return g_fallos == 0 ? 0 : 1;                                          \
  } while (0)
```

- [ ] **Step 2: Escribir el test que falla**

`firmware/tests_host/test_protocolo.cpp`:

```cpp
#include "../comun/protocolo_aura.h"
#include "aserciones.h"

int main() {
  const uint8_t A[6] = {0xAA,0,0,0,0,1};
  const uint8_t B[6] = {0xBB,0,0,0,0,2};

  // La trama entra en el limite de ESP-NOW
  VERIFICAR(sizeof(TramaAura) == 197);
  VERIFICAR(sizeof(TramaAura) <= 250);
  VERIFICAR(AURA_CABECERA_BYTES == 17);

  // init deja la trama consistente
  TramaAura t;
  const uint8_t datos[3] = {10, 20, 30};
  aura_trama_init(&t, AURA_TIPO_TELEMETRIA, A, B, 7, datos, 3);
  VERIFICAR(t.version == AURA_PROTO_VERSION);
  VERIFICAR(t.tipo == AURA_TIPO_TELEMETRIA);
  VERIFICAR(t.seq == 7);
  VERIFICAR(t.largo == 3);
  VERIFICAR(memcmp(t.mac_origen, A, 6) == 0);
  VERIFICAR(memcmp(t.mac_destino, B, 6) == 0);
  VERIFICAR(t.payload[2] == 30);

  // Solo se transmiten los bytes utiles, no los 197 completos
  VERIFICAR(aura_trama_bytes(&t) == 20);

  // Validacion de lo que llega por la radio
  VERIFICAR(aura_trama_valida((const uint8_t*)&t, 20) == true);
  VERIFICAR(aura_trama_valida((const uint8_t*)&t, 16) == false);  // mas corta que la cabecera
  VERIFICAR(aura_trama_valida((const uint8_t*)&t, 19) == false);  // largo no coincide

  TramaAura mala = t;
  mala.version = 99;
  VERIFICAR(aura_trama_valida((const uint8_t*)&mala, 20) == false);

  TramaAura larga = t;
  larga.largo = 200;  // mayor que AURA_PAYLOAD_MAX
  VERIFICAR(aura_trama_valida((const uint8_t*)&larga, 20) == false);

  // Direccionamiento: destino final, no proximo salto
  VERIFICAR(aura_es_para_mi(&t, B) == true);
  VERIFICAR(aura_es_para_mi(&t, A) == false);

  RESUMEN();
}
```

- [ ] **Step 3: Escribir el Makefile**

`firmware/tests_host/Makefile`:

```make
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g

TESTS = test_protocolo test_buffer test_ingest_id

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do \
	  echo "== $$t =="; ./$$t || exit 1; \
	done

test_protocolo: test_protocolo.cpp ../comun/protocolo_aura.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_buffer: test_buffer.cpp ../comun/buffer_circular.h ../comun/protocolo_aura.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_ingest_id: test_ingest_id.cpp ../comun/ingest_id.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)
```

Nota: el target `test` corre los tres tests. En esta tarea solo existe el primero, así que
usá `make test_protocolo && ./test_protocolo` hasta terminar la Task 3.

- [ ] **Step 4: Correr el test y verificar que falla**

```bash
cd firmware/tests_host && make test_protocolo
```

Esperado: FALLA la compilación con `protocolo_aura.h: No such file or directory`.

- [ ] **Step 5: Implementar el header**

`firmware/comun/protocolo_aura.h`:

```c
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define AURA_PROTO_VERSION 1
#define AURA_PAYLOAD_MAX   180
#define AURA_CABECERA_BYTES 17

// Tipos de mensaje. Se transmiten como uint8_t, no cambiar los valores.
enum {
  AURA_TIPO_TELEMETRIA = 1,
  AURA_TIPO_COMANDO    = 2,
  AURA_TIPO_ACK        = 3
};

// __attribute__((packed)) evita relleno entre campos: emisor y receptor
// tienen que interpretar exactamente los mismos bytes.
typedef struct __attribute__((packed)) {
  uint8_t  version;
  uint8_t  tipo;
  uint8_t  mac_origen[6];    // quien genero el mensaje
  uint8_t  mac_destino[6];   // destinatario FINAL, no el proximo salto
  uint16_t seq;              // secuencia por nodo origen, para ACK e ingest_id
  uint8_t  largo;            // bytes utiles en payload
  uint8_t  payload[AURA_PAYLOAD_MAX];
} TramaAura;

static inline void aura_trama_init(TramaAura* t, uint8_t tipo,
                                   const uint8_t origen[6],
                                   const uint8_t destino[6],
                                   uint16_t seq,
                                   const uint8_t* payload, uint8_t largo) {
  memset(t, 0, sizeof(*t));
  t->version = AURA_PROTO_VERSION;
  t->tipo    = tipo;
  memcpy(t->mac_origen,  origen,  6);
  memcpy(t->mac_destino, destino, 6);
  t->seq   = seq;
  t->largo = (largo > AURA_PAYLOAD_MAX) ? AURA_PAYLOAD_MAX : largo;
  if (payload && t->largo) memcpy(t->payload, payload, t->largo);
}

// Cuantos bytes hay que mandar realmente por la radio.
static inline uint16_t aura_trama_bytes(const TramaAura* t) {
  return (uint16_t)(AURA_CABECERA_BYTES + t->largo);
}

// Valida lo que llega del aire ANTES de interpretarlo.
static inline bool aura_trama_valida(const uint8_t* datos, int len) {
  if (datos == NULL) return false;
  if (len < AURA_CABECERA_BYTES) return false;
  const TramaAura* t = (const TramaAura*)datos;
  if (t->version != AURA_PROTO_VERSION) return false;
  if (t->largo > AURA_PAYLOAD_MAX) return false;
  if (len != AURA_CABECERA_BYTES + t->largo) return false;
  return true;
}

static inline bool aura_es_para_mi(const TramaAura* t, const uint8_t mi_mac[6]) {
  return memcmp(t->mac_destino, mi_mac, 6) == 0;
}
```

- [ ] **Step 6: Correr el test y verificar que pasa**

```bash
cd firmware/tests_host && make test_protocolo && ./test_protocolo
```

Esperado: `0 fallos` y código de salida 0.

- [ ] **Step 7: Commit**

```bash
git add firmware/comun/protocolo_aura.h firmware/tests_host/
git commit -m "feat(firmware): trama ESP-NOW de AURA con validacion y tests de host"
```

---

### Task 2: Buffer circular con descarte del más viejo

**Files:**
- Create: `firmware/comun/buffer_circular.h`
- Create: `firmware/tests_host/test_buffer.cpp`

**Interfaces:**
- Consumes: `TramaAura` de Task 1.
- Produces: `BufferCircular`, `AURA_BUFFER_CAP`, `buffer_init()`, `buffer_push()`,
  `buffer_peek()`, `buffer_pop()`, `buffer_cantidad()`, `buffer_descartados()`.

Este es el componente donde más fácil se cuelan errores (índices circulares), y el que
sostiene toda la tolerancia a cortes de la cadena. Por eso se prueba exhaustivamente en host.

- [ ] **Step 1: Escribir el test que falla**

`firmware/tests_host/test_buffer.cpp`:

```cpp
#include "../comun/buffer_circular.h"
#include "aserciones.h"

static TramaAura hacer(uint16_t seq) {
  const uint8_t A[6] = {0xAA,0,0,0,0,1};
  const uint8_t B[6] = {0xBB,0,0,0,0,2};
  TramaAura t;
  aura_trama_init(&t, AURA_TIPO_TELEMETRIA, A, B, seq, NULL, 0);
  return t;
}

int main() {
  BufferCircular b;
  buffer_init(&b);

  // Arranca vacio
  VERIFICAR(buffer_cantidad(&b) == 0);
  VERIFICAR(buffer_descartados(&b) == 0);
  TramaAura salida;
  VERIFICAR(buffer_pop(&b, &salida) == false);

  // FIFO: sale en el orden que entro
  TramaAura t1 = hacer(1), t2 = hacer(2);
  buffer_push(&b, &t1);
  buffer_push(&b, &t2);
  VERIFICAR(buffer_cantidad(&b) == 2);
  VERIFICAR(buffer_pop(&b, &salida) == true);
  VERIFICAR(salida.seq == 1);
  VERIFICAR(buffer_pop(&b, &salida) == true);
  VERIFICAR(salida.seq == 2);
  VERIFICAR(buffer_cantidad(&b) == 0);

  // peek no consume
  buffer_init(&b);
  TramaAura t42 = hacer(42);
  buffer_push(&b, &t42);
  VERIFICAR(buffer_peek(&b, &salida) == true);
  VERIFICAR(salida.seq == 42);
  VERIFICAR(buffer_cantidad(&b) == 1);

  // Llenar exactamente hasta la capacidad no descarta nada
  buffer_init(&b);
  for (uint16_t i = 0; i < AURA_BUFFER_CAP; i++) { TramaAura x = hacer(i); buffer_push(&b, &x); }
  VERIFICAR(buffer_cantidad(&b) == AURA_BUFFER_CAP);
  VERIFICAR(buffer_descartados(&b) == 0);

  // Pasarse pisa el MAS VIEJO y lo cuenta
  TramaAura t999 = hacer(999);
  buffer_push(&b, &t999);
  VERIFICAR(buffer_cantidad(&b) == AURA_BUFFER_CAP);
  VERIFICAR(buffer_descartados(&b) == 1);
  VERIFICAR(buffer_peek(&b, &salida) == true);
  VERIFICAR(salida.seq == 1);  // el 0 se perdio, ahora el mas viejo es el 1

  // El nuevo quedo al final
  for (uint16_t i = 0; i < AURA_BUFFER_CAP - 1; i++) buffer_pop(&b, &salida);
  VERIFICAR(buffer_pop(&b, &salida) == true);
  VERIFICAR(salida.seq == 999);
  VERIFICAR(buffer_cantidad(&b) == 0);

  // Uso prolongado: el indice circular no se desalinea
  buffer_init(&b);
  for (uint16_t i = 0; i < 500; i++) {
    TramaAura x = hacer(i);
    buffer_push(&b, &x);
    VERIFICAR(buffer_pop(&b, &salida) == true);
    VERIFICAR(salida.seq == i);
  }
  VERIFICAR(buffer_cantidad(&b) == 0);
  VERIFICAR(buffer_descartados(&b) == 0);

  RESUMEN();
}
```

- [ ] **Step 2: Correr el test y verificar que falla**

```bash
cd firmware/tests_host && make test_buffer
```

Esperado: FALLA con `buffer_circular.h: No such file or directory`.

- [ ] **Step 3: Implementar el header**

`firmware/comun/buffer_circular.h`:

```c
#pragma once
#include "protocolo_aura.h"

// 30 tramas x 197 bytes = 5910 bytes de RAM. El ESP32 tiene ~320 KB.
#define AURA_BUFFER_CAP 30

typedef struct {
  TramaAura items[AURA_BUFFER_CAP];
  uint16_t  inicio;       // indice del mas viejo
  uint16_t  cantidad;
  uint32_t  descartados;  // metrica: cuantos se perdieron por buffer lleno
} BufferCircular;

static inline void buffer_init(BufferCircular* b) {
  b->inicio = 0;
  b->cantidad = 0;
  b->descartados = 0;
}

static inline uint16_t buffer_cantidad(const BufferCircular* b) { return b->cantidad; }
static inline uint32_t buffer_descartados(const BufferCircular* b) { return b->descartados; }

// Si esta lleno, pisa el mas viejo. Nunca falla: perder la muestra mas
// antigua es preferible a rechazar la mas reciente.
static inline void buffer_push(BufferCircular* b, const TramaAura* t) {
  if (b->cantidad == AURA_BUFFER_CAP) {
    b->inicio = (uint16_t)((b->inicio + 1) % AURA_BUFFER_CAP);
    b->cantidad--;
    b->descartados++;
  }
  uint16_t fin = (uint16_t)((b->inicio + b->cantidad) % AURA_BUFFER_CAP);
  b->items[fin] = *t;
  b->cantidad++;
}

static inline bool buffer_peek(const BufferCircular* b, TramaAura* salida) {
  if (b->cantidad == 0) return false;
  *salida = b->items[b->inicio];
  return true;
}

static inline bool buffer_pop(BufferCircular* b, TramaAura* salida) {
  if (b->cantidad == 0) return false;
  *salida = b->items[b->inicio];
  b->inicio = (uint16_t)((b->inicio + 1) % AURA_BUFFER_CAP);
  b->cantidad--;
  return true;
}
```

- [ ] **Step 4: Correr el test y verificar que pasa**

```bash
cd firmware/tests_host && make test_buffer && ./test_buffer
```

Esperado: `0 fallos`.

- [ ] **Step 5: Commit**

```bash
git add firmware/comun/buffer_circular.h firmware/tests_host/test_buffer.cpp
git commit -m "feat(firmware): buffer circular con descarte del mas viejo"
```

---

### Task 3: ingest_id determinístico

**Files:**
- Create: `firmware/comun/ingest_id.h`
- Create: `firmware/tests_host/test_ingest_id.cpp`

**Interfaces:**
- Consumes: nada.
- Produces: `aura_ingest_id(const uint8_t mac[6], uint32_t boot_id, uint16_t seq, char salida[37])`.

Esta es la pieza que hace segura la retransmisión. Si el gateway reintenta un lote, el
backend tiene que reconocer los eventos como los mismos y descartarlos por el índice único
`idx_ts_telemetry_ingest_ux`. Para eso el UUID debe depender **solo** de `(mac, boot_id, seq)`
y nunca del momento del reintento.

Nota: el UUID se arma con formato válido 8-4-4-4-12 pero no se fuerzan los bits de versión
RFC 4122. Pydantic acepta cualquier hexadecimal con ese formato, y forzar los bits
sacrificaría bytes de identidad reales.

- [ ] **Step 1: Escribir el test que falla**

`firmware/tests_host/test_ingest_id.cpp`:

```cpp
#include "../comun/ingest_id.h"
#include "aserciones.h"
#include <cstring>

int main() {
  const uint8_t mac1[6] = {0x24,0x6F,0x28,0x11,0x22,0x33};
  const uint8_t mac2[6] = {0x24,0x6F,0x28,0x11,0x22,0x34};
  char a[37], b[37];

  // Formato: 36 caracteres, guiones en 8-13-18-23
  aura_ingest_id(mac1, 1000, 5, a);
  VERIFICAR(strlen(a) == 36);
  VERIFICAR(a[8] == '-' && a[13] == '-' && a[18] == '-' && a[23] == '-');
  for (int i = 0; i < 36; i++) {
    bool ok = (a[i] == '-') || (a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f');
    VERIFICAR(ok);
  }

  // DETERMINISTICO: la misma entrada da siempre el mismo id.
  // Sin esto, reintentar duplica filas en ts_telemetry.
  aura_ingest_id(mac1, 1000, 5, b);
  VERIFICAR(strcmp(a, b) == 0);

  // Cambiar cualquiera de los tres componentes cambia el id
  aura_ingest_id(mac1, 1000, 6, b);
  VERIFICAR(strcmp(a, b) != 0);
  aura_ingest_id(mac1, 1001, 5, b);
  VERIFICAR(strcmp(a, b) != 0);
  aura_ingest_id(mac2, 1000, 5, b);
  VERIFICAR(strcmp(a, b) != 0);

  // Dos nodos distintos con la misma seq no colisionan
  aura_ingest_id(mac1, 7, 1, a);
  aura_ingest_id(mac2, 7, 1, b);
  VERIFICAR(strcmp(a, b) != 0);

  RESUMEN();
}
```

- [ ] **Step 2: Correr el test y verificar que falla**

```bash
cd firmware/tests_host && make test_ingest_id
```

Esperado: FALLA con `ingest_id.h: No such file or directory`.

- [ ] **Step 3: Implementar el header**

`firmware/comun/ingest_id.h`:

```c
#pragma once
#include <stdint.h>
#include <stdio.h>

// FNV-1a de 32 bits. Solo se usa para llenar los ultimos 4 bytes del UUID
// con algo que dependa de todo lo anterior; no es criptografico.
static inline uint32_t aura_fnv1a(const uint8_t* datos, int len) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < len; i++) {
    h ^= datos[i];
    h *= 16777619u;
  }
  return h;
}

// Arma un UUID determinístico a partir de (mac, boot_id, seq).
// La misma terna produce siempre la misma cadena: eso es lo que permite
// reintentar sin duplicar en el backend.
// salida debe tener al menos 37 bytes (36 + terminador).
static inline void aura_ingest_id(const uint8_t mac[6], uint32_t boot_id,
                                  uint16_t seq, char salida[37]) {
  uint8_t b[16];
  for (int i = 0; i < 6; i++) b[i] = mac[i];
  b[6]  = (uint8_t)(boot_id >> 24);
  b[7]  = (uint8_t)(boot_id >> 16);
  b[8]  = (uint8_t)(boot_id >> 8);
  b[9]  = (uint8_t)(boot_id);
  b[10] = (uint8_t)(seq >> 8);
  b[11] = (uint8_t)(seq);

  uint32_t h = aura_fnv1a(b, 12);
  b[12] = (uint8_t)(h >> 24);
  b[13] = (uint8_t)(h >> 16);
  b[14] = (uint8_t)(h >> 8);
  b[15] = (uint8_t)(h);

  snprintf(salida, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
           b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
}
```

- [ ] **Step 4: Correr el test y verificar que pasa**

```bash
cd firmware/tests_host && make test_ingest_id && ./test_ingest_id
```

Esperado: `0 fallos`.

- [ ] **Step 5: Correr la suite completa**

```bash
cd firmware/tests_host && make clean && make test
```

Esperado: los tres tests con 0 fallos.

- [ ] **Step 6: Commit**

```bash
git add firmware/comun/ingest_id.h firmware/tests_host/test_ingest_id.cpp
git commit -m "feat(firmware): ingest_id deterministico para reintentos idempotentes"
```

---

### Task 4: nodo_sensor

**Files:**
- Create: `firmware/nodo_sensor/nodo_sensor.ino`

**Interfaces:**
- Consumes: `protocolo_aura.h`, `buffer_circular.h`.
- Produces: nada para otras tareas. Es una hoja.

**Requisito previo:** copiar `firmware/comun/*.h` junto al `.ino` o agregar `comun/` a las
rutas de include del IDE. El IDE de Arduino solo compila headers que estén en la carpeta
del sketch o en `libraries/`.

- [ ] **Step 1: Escribir el sketch**

```cpp
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <BH1750.h>
#include "protocolo_aura.h"
#include "buffer_circular.h"

// MAC del nodo de sala. Cambiar por la real antes de flashear.
uint8_t MAC_PADRE[6] = {0x00,0x00,0x00,0x00,0x00,0x00};

const unsigned long INTERVALO_MUESTRA = 10000;  // 10 s
const unsigned long TIMEOUT_ACK       = 1500;

BH1750 sensor;
BufferCircular pendientes;
uint8_t  mi_mac[6];
uint16_t seq_actual = 0;
uint32_t boot_id    = 0;

volatile bool  ack_recibido = false;
volatile uint16_t ack_seq   = 0;

unsigned long ultima_muestra = 0;

void on_recv(const esp_now_recv_info_t* info, const uint8_t* datos, int len) {
  (void)info;
  if (!aura_trama_valida(datos, len)) return;
  const TramaAura* t = (const TramaAura*)datos;
  if (t->tipo == AURA_TIPO_ACK && aura_es_para_mi(t, mi_mac)) {
    ack_seq = t->seq;
    ack_recibido = true;
  }
}

// Envia una trama y espera el ACK del padre. Devuelve si fue confirmada.
bool enviar_con_ack(const TramaAura* t) {
  ack_recibido = false;
  if (esp_now_send(MAC_PADRE, (const uint8_t*)t, aura_trama_bytes(t)) != ESP_OK) return false;

  unsigned long inicio = millis();
  while (millis() - inicio < TIMEOUT_ACK) {
    if (ack_recibido && ack_seq == t->seq) return true;
    delay(10);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  sensor.begin();

  WiFi.mode(WIFI_STA);
  // Sin setSleep(false) el receptor se apaga por intervalos y se pierden
  // los ACK del padre aunque el envio funcione.
  WiFi.setSleep(false);
  esp_wifi_get_mac(WIFI_IF_STA, mi_mac);

  boot_id = esp_random();
  buffer_init(&pendientes);

  if (esp_now_init() != ESP_OK) { Serial.println("fallo esp_now_init"); ESP.restart(); }
  esp_now_register_recv_cb(on_recv);

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, MAC_PADRE, 6);
  peer.channel = 0;   // 0 = canal actual, impuesto por el AP
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Serial.println("nodo_sensor listo");
}

void loop() {
  unsigned long ahora = millis();

  if (ahora - ultima_muestra >= INTERVALO_MUESTRA) {
    ultima_muestra = ahora;

    float lux = sensor.readLightLevel();
    char json[64];
    int n = snprintf(json, sizeof(json), "{\"lux\":%.1f}", lux);

    TramaAura t;
    aura_trama_init(&t, AURA_TIPO_TELEMETRIA, mi_mac, MAC_PADRE,
                    seq_actual++, (const uint8_t*)json, (uint8_t)n);
    buffer_push(&pendientes, &t);
    Serial.printf("muestra %.1f lux, pendientes=%u descartados=%lu\n",
                  lux, buffer_cantidad(&pendientes),
                  (unsigned long)buffer_descartados(&pendientes));
  }

  // Drenar el buffer: solo se saca lo que el padre confirmo.
  TramaAura t;
  if (buffer_peek(&pendientes, &t)) {
    if (enviar_con_ack(&t)) {
      buffer_pop(&pendientes, &t);
    } else {
      delay(500);  // backoff antes del proximo intento
    }
  }

  delay(50);
}
```

- [ ] **Step 2: Compilar**

Placa: ESP32 Dev Module. Verificar que compila sin warnings.
Si aparece `esp_now_recv_info_t no declarado`, el core instalado no es 3.x — revisar
Herramientas → Placa → Gestor de tarjetas.

- [ ] **Step 3: Verificar en banco (sin el padre todavía)**

Flashear y abrir el monitor a 115200. Esperado: aparece una muestra cada 10 s, `pendientes`
sube hasta 30 y ahí se queda mientras `descartados` empieza a subir. **Esto es el
comportamiento correcto**: sin padre no hay ACK, nada se drena, y el buffer pisa lo viejo.

- [ ] **Step 4: Commit**

```bash
git add firmware/nodo_sensor/nodo_sensor.ino
git commit -m "feat(firmware): nodo sensor de luz con buffer y ACK del padre"
```

---

### Task 5: nodo_sala

**Files:**
- Create: `firmware/nodo_sala/nodo_sala.ino`

**Interfaces:**
- Consumes: `protocolo_aura.h`, `buffer_circular.h`.
- Produces: el comportamiento de reenvío del que depende el gateway (Task 6).

Es el único nodo con dos vecinos. La regla que evita lógica por caso: si
`mac_destino` es la propia, procesar; si no, reenviar al otro vecino.

- [ ] **Step 1: Escribir el sketch**

```cpp
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "protocolo_aura.h"
#include "buffer_circular.h"

// Cambiar por las MAC reales antes de flashear.
uint8_t MAC_SENSOR[6]  = {0x00,0x00,0x00,0x00,0x00,0x00};
uint8_t MAC_GATEWAY[6] = {0x00,0x00,0x00,0x00,0x00,0x00};

const unsigned long TIMEOUT_ACK = 1500;

BufferCircular hacia_arriba;
uint8_t  mi_mac[6];
uint16_t seq_actual = 0;

volatile bool     ack_recibido = false;
volatile uint16_t ack_seq      = 0;

float ultimo_lux = -1;

void mandar_ack(const uint8_t destino[6], uint16_t seq) {
  TramaAura ack;
  aura_trama_init(&ack, AURA_TIPO_ACK, mi_mac, destino, seq, NULL, 0);
  esp_now_send(destino, (const uint8_t*)&ack, aura_trama_bytes(&ack));
}

void on_recv(const esp_now_recv_info_t* info, const uint8_t* datos, int len) {
  (void)info;
  if (!aura_trama_valida(datos, len)) return;
  const TramaAura* t = (const TramaAura*)datos;

  if (t->tipo == AURA_TIPO_ACK) {
    if (aura_es_para_mi(t, mi_mac)) { ack_seq = t->seq; ack_recibido = true; }
    return;
  }

  if (t->tipo == AURA_TIPO_TELEMETRIA) {
    // Confirmar al sensor apenas se acepta, aunque todavia no se haya subido.
    mandar_ack(t->mac_origen, t->seq);
    // Mostrar de paso: la sala consume el dato ademas de reenviarlo.
    sscanf((const char*)t->payload, "{\"lux\":%f}", &ultimo_lux);
    buffer_push(&hacia_arriba, t);
    return;
  }

  if (t->tipo == AURA_TIPO_COMANDO) {
    mandar_ack(t->mac_origen, t->seq);
    if (aura_es_para_mi(t, mi_mac)) {
      aplicar_comando((const char*)t->payload, t->largo);
    } else {
      // No es para mi: bajarlo al sensor sin interpretarlo.
      esp_now_send(MAC_SENSOR, (const uint8_t*)t, aura_trama_bytes(t));
    }
  }
}

void aplicar_comando(const char* json, uint8_t largo) {
  (void)largo;
  Serial.printf("comando para esta sala: %s\n", json);
  // TODO de producto, no del plan: mapear a los actuadores reales de la sala.
}

bool enviar_con_ack(const uint8_t destino[6], const TramaAura* t) {
  ack_recibido = false;
  if (esp_now_send(destino, (const uint8_t*)t, aura_trama_bytes(t)) != ESP_OK) return false;
  unsigned long inicio = millis();
  while (millis() - inicio < TIMEOUT_ACK) {
    if (ack_recibido && ack_seq == t->seq) return true;
    delay(10);
  }
  return false;
}

void agregar_peer(const uint8_t mac[6]) {
  esp_now_peer_info_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_get_mac(WIFI_IF_STA, mi_mac);
  buffer_init(&hacia_arriba);

  if (esp_now_init() != ESP_OK) { Serial.println("fallo esp_now_init"); ESP.restart(); }
  esp_now_register_recv_cb(on_recv);
  agregar_peer(MAC_SENSOR);
  agregar_peer(MAC_GATEWAY);

  Serial.println("nodo_sala listo");
}

void loop() {
  TramaAura t;
  if (buffer_peek(&hacia_arriba, &t)) {
    if (enviar_con_ack(MAC_GATEWAY, &t)) buffer_pop(&hacia_arriba, &t);
    else delay(500);
  }

  static unsigned long ultimo_print = 0;
  if (millis() - ultimo_print > 5000) {
    ultimo_print = millis();
    Serial.printf("lux=%.1f  pendientes=%u  descartados=%lu\n",
                  ultimo_lux, buffer_cantidad(&hacia_arriba),
                  (unsigned long)buffer_descartados(&hacia_arriba));
  }
  delay(50);
}
```

- [ ] **Step 2: Compilar y flashear**

Antes de flashear, poner en `MAC_SENSOR` la MAC que imprime el nodo sensor al arrancar,
y en el sensor poner la MAC de esta placa como `MAC_PADRE`.

- [ ] **Step 3: Verificar la cadena de un salto**

Con sensor + sala encendidos, esperado en el monitor de la sala: `lux=` con un valor real
cambiando al tapar el sensor. En el monitor del sensor: `pendientes` vuelve a 0 después de
cada muestra, porque ahora hay ACK. **Que `pendientes` baje a 0 es la señal de que el ACK
funciona.**

- [ ] **Step 4: Commit**

```bash
git add firmware/nodo_sala/nodo_sala.ino
git commit -m "feat(firmware): nodo de sala con reenvio bidireccional por mac_destino"
```

---

### Task 6: nodo_gateway

**Files:**
- Create: `firmware/nodo_gateway/nodo_gateway.ino`

**Interfaces:**
- Consumes: `protocolo_aura.h`, `buffer_circular.h`, `ingest_id.h`.
- Produces: la traducción MAC→UUID y las publicaciones que consume el backend.

- [ ] **Step 1: Escribir el sketch**

```cpp
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>
#include "protocolo_aura.h"
#include "buffer_circular.h"
#include "ingest_id.h"

const char* WIFI_SSID = "CAMBIAR";
const char* WIFI_PASS = "CAMBIAR";
const char* MQTT_HOST = "192.168.0.16";
const int   MQTT_PORT = 1883;
const char* API_BASE  = "http://192.168.0.16:8000";
const char* TENANT_ID = "550e8400-e29b-41d4-a716-446655440000";  // CAMBIAR

// Tabla MAC -> UUID. Unica fuente de identidad de la mesh frente a AURA.
// Agregar un nodo implica reflashear el gateway (decision 3.2 del spec).
typedef struct {
  uint8_t     mac[6];
  const char* device_id;
  const char* tipo;     // campo "type" de TelemetryEventCreate
} NodoConocido;

NodoConocido TABLA[] = {
  {{0x00,0x00,0x00,0x00,0x00,0x00}, "650e8400-e29b-41d4-a716-446655440001", "lux"},
  {{0x00,0x00,0x00,0x00,0x00,0x00}, "650e8400-e29b-41d4-a716-446655440002", "sala"},
};
const int TABLA_N = sizeof(TABLA) / sizeof(TABLA[0]);

WiFiClient  net;
MqttClient  mqtt(net);
BufferCircular por_subir;
uint8_t  mi_mac[6];
uint16_t seq_actual = 0;
uint32_t boot_id = 0;

const NodoConocido* buscar(const uint8_t mac[6]) {
  for (int i = 0; i < TABLA_N; i++)
    if (memcmp(TABLA[i].mac, mac, 6) == 0) return &TABLA[i];
  return NULL;
}

void mandar_ack(const uint8_t destino[6], uint16_t seq) {
  TramaAura ack;
  aura_trama_init(&ack, AURA_TIPO_ACK, mi_mac, destino, seq, NULL, 0);
  esp_now_send(destino, (const uint8_t*)&ack, aura_trama_bytes(&ack));
}

void on_recv(const esp_now_recv_info_t* info, const uint8_t* datos, int len) {
  (void)info;
  if (!aura_trama_valida(datos, len)) return;
  const TramaAura* t = (const TramaAura*)datos;
  if (t->tipo != AURA_TIPO_TELEMETRIA) return;
  mandar_ack(t->mac_origen, t->seq);
  buffer_push(&por_subir, t);
}

// Sube un lote por REST. Idempotente: reintentar el mismo lote no duplica
// filas gracias al ingest_id deterministico.
bool subir_lote() {
  if (buffer_cantidad(&por_subir) == 0) return true;

  JsonDocument doc;
  JsonArray eventos = doc["events"].to<JsonArray>();

  int n = 0;
  // static: BufferCircular pesa ~5,9 KB y la loop task del ESP32 tiene 8 KB
  // de stack. Una copia local aca desborda el stack y reinicia la placa.
  static BufferCircular tmp;
  tmp = por_subir;  // se drena de verdad solo si el POST sale bien
  TramaAura t;
  while (buffer_pop(&tmp, &t) && n < 10) {
    const NodoConocido* nodo = buscar(t.mac_origen);
    if (!nodo) continue;  // MAC desconocida: se descarta, no se puede identificar

    char uuid[37];
    aura_ingest_id(t.mac_origen, boot_id, t.seq, uuid);

    JsonObject e = eventos.add<JsonObject>();
    e["tenant_id"] = TENANT_ID;
    e["device_id"] = nodo->device_id;
    e["type"]      = nodo->tipo;
    e["ingest_id"] = uuid;

    JsonDocument p;
    deserializeJson(p, (const char*)t.payload);
    e["payload"] = p;

    n++;
  }
  if (n == 0) return true;

  String cuerpo;
  serializeJson(doc, cuerpo);

  HTTPClient http;
  http.begin(String(API_BASE) + "/api/v1/telemetry/ingest");
  http.addHeader("Content-Type", "application/json");
  int codigo = http.POST(cuerpo);
  http.end();

  if (codigo == 201 || codigo == 200) {
    for (int i = 0; i < n; i++) buffer_pop(&por_subir, &t);
    Serial.printf("subidos %d eventos\n", n);
    return true;
  }
  Serial.printf("fallo el POST: %d, se reintenta\n", codigo);
  return false;
}

void on_mqtt(int size) {
  (void)size;
  String topico = mqtt.messageTopic();
  String msg;
  while (mqtt.available()) msg += (char)mqtt.read();

  // devices/{uuid}/command -> bajar a la MAC correspondiente
  for (int i = 0; i < TABLA_N; i++) {
    if (topico == "devices/" + String(TABLA[i].device_id) + "/command") {
      TramaAura t;
      aura_trama_init(&t, AURA_TIPO_COMANDO, mi_mac, TABLA[i].mac,
                      seq_actual++, (const uint8_t*)msg.c_str(), (uint8_t)msg.length());
      // Al nodo de sala; si el destino es el sensor, la sala lo reenvia.
      esp_now_send(TABLA[1].mac, (const uint8_t*)&t, aura_trama_bytes(&t));
      Serial.println("comando bajado a la mesh");
      return;
    }
  }
}

void publicar_estado() {
  uint8_t canal; wifi_second_chan_t s;
  esp_wifi_get_channel(&canal, &s);
  for (int i = 0; i < TABLA_N; i++) {
    String top = "devices/" + String(TABLA[i].device_id) + "/status";
    mqtt.beginMessage(top.c_str(), true, 1);
    mqtt.print("{\"status\":\"online\",\"canal\":" + String(canal) + "}");
    mqtt.endMessage();
  }
}

void setup() {
  Serial.begin(115200);
  buffer_init(&por_subir);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  WiFi.setSleep(false);
  esp_wifi_get_mac(WIFI_IF_STA, mi_mac);
  boot_id = esp_random();

  uint8_t canal; wifi_second_chan_t s;
  esp_wifi_get_channel(&canal, &s);
  Serial.printf("\nWiFi ok, canal del AP=%d. Los nodos hoja deben estar aca.\n", canal);

  if (esp_now_init() != ESP_OK) { Serial.println("fallo esp_now_init"); ESP.restart(); }
  esp_now_register_recv_cb(on_recv);
  for (int i = 0; i < TABLA_N; i++) {
    esp_now_peer_info_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, TABLA[i].mac, 6);
    p.channel = 0;
    p.encrypt = false;
    esp_now_add_peer(&p);
  }

  mqtt.onMessage(on_mqtt);
  if (!mqtt.connect(MQTT_HOST, MQTT_PORT)) { Serial.println("fallo MQTT"); }
  for (int i = 0; i < TABLA_N; i++)
    mqtt.subscribe(("devices/" + String(TABLA[i].device_id) + "/command").c_str(), 1);
  publicar_estado();
}

void loop() {
  mqtt.poll();
  static unsigned long ultimo = 0;
  if (millis() - ultimo > 5000) { ultimo = millis(); subir_lote(); }
  delay(50);
}
```

- [ ] **Step 2: Compilar y configurar**

Completar `WIFI_SSID`, `WIFI_PASS`, `MQTT_HOST`, `API_BASE`, `TENANT_ID` y las dos MAC de
`TABLA`. Los `device_id` deben ser UUID de filas que **ya existan** en la tabla `devices`
del backend, con su `mac_address` cargada.

- [ ] **Step 3: Verificar la cadena completa**

Con los tres nodos y el stack levantado (`make up` en la raíz del repo principal):

```bash
curl "http://localhost:8000/api/v1/telemetry/?device_id=<uuid-del-sensor>&limit=5"
```

Esperado: filas con el valor de lux. Tapar el sensor y confirmar que el siguiente valor baja.

- [ ] **Step 4: Verificar la idempotencia (el test que justifica todo el diseño)**

1. Anotar el conteo: `curl ".../api/v1/telemetry/stats?device_id=<uuid>"`.
2. Apagar el gateway 60 s con el sensor emitiendo.
3. Encenderlo y esperar a que drene.
4. Volver a contar.

Esperado: el conteo sube por las muestras acumuladas, **sin filas repetidas**. Si aparecen
duplicados, el `ingest_id` no está siendo determinístico — revisar que `boot_id` del gateway
no entre en el cálculo de tramas ajenas.

- [ ] **Step 5: Verificar el downlink**

```bash
mosquitto_pub -h <broker> -t "devices/<uuid-de-sala>/command" -m '{"accion":"test"}'
```

Esperado: en el monitor de la sala aparece `comando para esta sala: {"accion":"test"}`.

- [ ] **Step 6: Commit**

```bash
git add firmware/nodo_gateway/nodo_gateway.ino
git commit -m "feat(firmware): gateway ESP-NOW a MQTT y REST con tabla MAC-UUID"
```

---

### Task 7: Recuperación de canal en nodos hoja

**Files:**
- Modify: `firmware/nodo_sensor/nodo_sensor.ino`
- Modify: `firmware/nodo_sala/nodo_sala.ino`

Implementa la mitigación de la §4 del spec. **No es opcional:** sin esto, un cambio de canal
del AP deja la mesh muda mientras el gateway sigue reportándose online contra AURA — el modo
de falla más difícil de diagnosticar del sistema.

- [ ] **Step 1: Agregar el barrido a ambos sketches**

Insertar en los dos `.ino`, y llamarlo desde `loop()`:

```cpp
const unsigned long SIN_ACK_MAX = 60000;  // 1 minuto sin confirmaciones
unsigned long ultimo_ack_ok = 0;

// Recorre los 13 canales buscando al padre. Al encontrarlo, se queda ahi.
void buscar_padre_por_canales() {
  Serial.println("enlace perdido, barriendo canales...");
  for (uint8_t canal = 1; canal <= 13; canal++) {
    esp_wifi_set_channel(canal, WIFI_SECOND_CHAN_NONE);
    delay(120);

    TramaAura ping;
    aura_trama_init(&ping, AURA_TIPO_TELEMETRIA, mi_mac, MAC_PADRE, seq_actual, NULL, 0);
    ack_recibido = false;
    esp_now_send(MAC_PADRE, (const uint8_t*)&ping, aura_trama_bytes(&ping));

    unsigned long inicio = millis();
    while (millis() - inicio < 300) {
      if (ack_recibido) {
        Serial.printf("padre encontrado en canal %d\n", canal);
        ultimo_ack_ok = millis();
        return;
      }
      delay(10);
    }
  }
  Serial.println("no se encontro al padre en ningun canal");
}
```

En `enviar_con_ack()`, al confirmar, agregar `ultimo_ack_ok = millis();`.
En `loop()`, agregar:

```cpp
if (ultimo_ack_ok != 0 && millis() - ultimo_ack_ok > SIN_ACK_MAX) {
  buscar_padre_por_canales();
}
```

En `nodo_sala.ino` usar `MAC_GATEWAY` en lugar de `MAC_PADRE`.

- [ ] **Step 2: Verificar**

Con la cadena funcionando, cambiar el canal del AP desde su configuración. Esperado: los
nodos hoja reportan `enlace perdido, barriendo canales...` y en menos de ~2 minutos
`padre encontrado en canal N`, y la telemetría se reanuda sin pérdida (el buffer la sostuvo).

- [ ] **Step 3: Commit**

```bash
git add firmware/nodo_sensor/nodo_sensor.ino firmware/nodo_sala/nodo_sala.ino
git commit -m "feat(firmware): barrido de canales ante perdida del enlace"
```

---

## Cobertura del spec

| Sección del spec | Tarea |
|---|---|
| §3.1 ruta fija con padre configurado | 4, 5 |
| §3.2 solo el gateway conoce AURA / tabla MAC→UUID | 6 |
| §3.3 dos vías de subida (MQTT + REST) | 6 |
| §3.4 trama única bidireccional | 1, 5 |
| §3.5 buffer, reintento e ingest_id | 2, 3, 4, 5, 6 |
| §4 restricción de canal | 6 (publicación), 7 (recuperación) |
| §6 pruebas 1-5 | 1-3 (host), 5 (dos nodos), 6 (cadena, idempotencia, downlink), 7 (canal) |
