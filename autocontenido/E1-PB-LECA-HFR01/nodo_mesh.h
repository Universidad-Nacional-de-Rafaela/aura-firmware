#pragma once
// Biblioteca de nodo de la mesh de AURA (contrato v4.0, ESP-WIFI-MESH). Es lo
// que usa un dispositivo de un grupo para hablar con AURA por la mesh, sin
// saber nada de MQTT ni de UUID: el grupo escribe la medicion y la validacion
// de su configuracion, y esto se ocupa del resto. El nodo es una HOJA de la
// mesh: no reenvia trafico de otros.
//
//   - Cada medicion se guarda en flash con su ingest_id ANTES del primer envio
//     y sale de la cola solo cuando el raiz confirma que AURA la recibio (ack).
//     Sobrevive a cortes del raiz, del WiFi, de AURA y del propio nodo.
//   - set_config: valida TODO o no aplica nada, y devuelve el resultado con la
//     configuracion vigente, que es lo que el raiz publica como "aplicado".
//   - Alertas de sonda una vez por cambio, no una por medicion.
//
// Uso minimo (ver ejemplos/sensor_ejemplo):
//
//   #include "config_local.h"   // MESH_ID, MESH_CLAVE, MESH_CANAL, MAC_ESPERADA
//   #include "nodo_mesh.h"
//   NodoMeshCallbacks cb = {aplicar_config, describir_config, NULL, "red"};
//   nodo_mesh_iniciar(cb);      // en setup()
//   nodo_mesh_loop();           // en cada loop()
//   JsonDocument d; d["temp_c"] = 4.5; nodo_mesh_medicion(d.as<JsonObjectConst>());
//
// El nodo no conoce su device_id ni la MAC del raiz: AURA lo identifica por la
// MAC de su placa (hw_id "mac-...") y la mesh encuentra sola el camino al raiz.
//
// Dependencias: ArduinoJson 7 y LittleFS (incluido en el core). Usa la
// particion de datos de la placa ("spiffs" en el esquema por defecto).

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <sys/time.h>
#include <time.h>
#include "protocolo_aura.h"
#include "radio_wifi_mesh.h"
#include "cola_persistente.h"
#include "buffer_circular.h"
#include "ingest_id.h"
#include "nodo_mesh_logica.h"

// ===== Configuracion de la mesh (config_local.h, que no se versiona) =====
// En cero: compila, mide y guarda, pero no se une a ninguna mesh.
#ifndef MESH_ID
#define MESH_ID {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#endif
#ifndef MESH_CLAVE
#define MESH_CLAVE ""
#endif
#ifndef MESH_CANAL
#define MESH_CANAL 0
#endif
#ifndef MESH_ROUTER_SSID
#define MESH_ROUTER_SSID ""
#endif
#ifndef MAC_ESPERADA
#define MAC_ESPERADA {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#endif

// ===== Lo que aporta el grupo =====

// Valida params contra sus rangos y, solo si TODO valida, lo aplica y lo guarda
// en memoria no volatil (Preferences). Si algo no valida, no toca nada,
// devuelve false y explica en motivo. NULL = el nodo no tiene parametros.
typedef bool (*NodoAplicarConfig)(JsonObjectConst params, String& motivo);

// Escribe en out la configuracion vigente. Incluir "intervalo_s": con eso el
// raiz infiere que el nodo se cayo (contrato §3.2). NULL = sin configuracion.
typedef void (*NodoDescribirConfig)(JsonObject out);

// Comandos propios del dispositivo, ademas de set_config (un actuador, por
// ejemplo). true = ejecutado. NULL = el nodo no tiene otros comandos.
typedef bool (*NodoEjecutarComando)(const char* comando, JsonObjectConst params, String& motivo);

typedef struct {
  NodoAplicarConfig   aplicar_config;
  NodoDescribirConfig describir_config;
  NodoEjecutarComando ejecutar_comando;
  const char*         alimentacion;  // "red", "bateria" o "desconocida"
} NodoMeshCallbacks;

// ===== Estado interno =====
#define NODO_MESH_ARCHIVO_COLA "/cola.bin"
#define NODO_MESH_REINTENTO_SALTO_MS 2000UL   // la mesh no acepto el envio: reintento corto
#define NODO_MESH_INTENTOS_EVENTO 3

typedef struct {
  bool     configurada;       // MESH_ID distinto de cero
  NodoMeshCallbacks cb;
  ColaPersistente cola;
  bool     cola_ok;
  BufferCircular eventos;     // resultado, reporte y alertas: en RAM, con pocos reintentos
  uint8_t  intentos_evento;
  uint16_t seq;
  // Muestra en vuelo: enviada al raiz, esperando la confirmacion de AURA.
  bool     en_vuelo;
  uint8_t  id_en_vuelo[AURA_INGEST_ID_BYTES];
  unsigned long enviada_en;
  uint32_t intentos;
  unsigned long proximo_envio;
  // Ultimo comando procesado: una retransmision no se ejecuta dos veces.
  bool     hay_ultimo_comando;
  uint16_t ultimo_comando_seq;
  EstadoSondas sondas;
  const char* alimentacion;
  uint32_t confirmadas;
} NodoMesh;

static NodoMesh nodo_mesh;

// ===== Almacenamiento de la cola en LittleFS =====
// Abrir y cerrar en cada operacion: cerrar es lo que asegura que quedo escrito
// en flash. A una muestra por minuto, el costo no importa.
static inline bool nodo_mesh_fs_leer(void* ctx, uint32_t off, void* buf, uint32_t n) {
  (void)ctx;
  File f = LittleFS.open(NODO_MESH_ARCHIVO_COLA, "r");
  if (!f) return false;
  bool ok = f.seek(off) && f.read((uint8_t*)buf, n) == n;
  f.close();
  return ok;
}

static inline bool nodo_mesh_fs_escribir(void* ctx, uint32_t off, const void* buf, uint32_t n) {
  (void)ctx;
  File f = LittleFS.open(NODO_MESH_ARCHIVO_COLA, "r+");
  if (!f) return false;
  bool ok = f.seek(off) && f.write((const uint8_t*)buf, n) == n;
  f.close();
  return ok;
}

// El archivo se crea una vez con su tamano final, para no escribir mas alla
// del final despues.
static inline bool nodo_mesh_fs_preparar() {
  if (!LittleFS.begin(true)) return false;
  uint32_t necesario = cola_bytes(AURA_COLA_CAP);
  File f = LittleFS.open(NODO_MESH_ARCHIVO_COLA, "r");
  uint32_t actual = f ? f.size() : 0;
  if (f) f.close();
  if (actual >= necesario) return true;

  Serial.printf("[MESH] creando la cola en flash (%lu bytes)...\n", (unsigned long)necesario);
  f = LittleFS.open(NODO_MESH_ARCHIVO_COLA, actual ? "a" : "w");
  if (!f) return false;
  uint8_t ceros[512];
  memset(ceros, 0, sizeof(ceros));
  for (uint32_t escrito = actual; escrito < necesario; ) {
    uint32_t n = necesario - escrito < sizeof(ceros) ? necesario - escrito : sizeof(ceros);
    if (f.write(ceros, n) != n) { f.close(); return false; }
    escrito += n;
  }
  f.close();
  return true;
}

// ===== Hora =====
static inline uint32_t nodo_mesh_ahora() {
  time_t t = time(NULL);
  return (t > 0 && aura_hora_valida((uint32_t)t)) ? (uint32_t)t : 0;
}

static inline uint32_t nodo_mesh_azar() { return esp_random(); }

// ===== Eventos (resultado, reporte, alerta) =====
// Van en RAM: si se pierden por un corte, el proximo reporte vuelve a dejar
// todo en orden. Se reintentan pocas veces para no trabar la cola de muestras.
static inline bool nodo_mesh_encolar_evento(uint8_t tipo, JsonDocument& doc) {
  char json[AURA_PAYLOAD_MAX + 1];
  size_t n = measureJson(doc);
  if (n > AURA_PAYLOAD_MAX) {
    Serial.printf("[MESH] !! evento de %u bytes, el maximo es %u: no se envia\n",
                  (unsigned)n, AURA_PAYLOAD_MAX);
    return false;
  }
  serializeJson(doc, json, sizeof(json));
  TramaAura t;
  aura_trama_init(&t, tipo, nodo_mesh.seq++, (const uint8_t*)json, (uint8_t)n);
  buffer_push(&nodo_mesh.eventos, &t);
  return true;
}

static inline void nodo_mesh_describir(JsonObject config) {
  if (nodo_mesh.cb.describir_config) nodo_mesh.cb.describir_config(config);
}

// Reporte de estado (contrato §3.2): al arrancar, al reconectar y despues de
// aplicar una configuracion.
static inline void nodo_mesh_reportar() {
  JsonDocument d;
  nodo_mesh_describir(d["config"].to<JsonObject>());
  d["pendientes"]   = nodo_mesh.cola_ok ? cola_cantidad(&nodo_mesh.cola) : 0;
  d["descartadas"]  = nodo_mesh.cola_ok ? cola_descartadas(&nodo_mesh.cola) : 0;
  d["alimentacion"] = nodo_mesh.alimentacion ? nodo_mesh.alimentacion : "desconocida";
  nodo_mesh_encolar_evento(AURA_TIPO_REPORTE, d);
}

// Alerta generica: el raiz la publica en hw/<hw_id>/alerts/<tipo>.
static inline void nodo_mesh_alerta(const char* tipo, const char* severity, const char* message,
                             JsonObjectConst details) {
  JsonDocument d;
  d["tipo"] = tipo;
  d["severity"] = severity;
  d["message"] = message;
  d["details"] = details;
  nodo_mesh_encolar_evento(AURA_TIPO_ALERTA, d);
}

// Informa el estado de una sonda en cada medicion. Manda la alerta "sensor"
// solo cuando cambia: al fallar (motivo "sin_respuesta" o "fuera_de_rango") y
// al recuperarse (contrato §3.5).
static inline void nodo_mesh_sonda(const char* campo, bool ok, const char* motivo_falla = "sin_respuesta") {
  AuraCambioSonda c = aura_sonda_cambio(&nodo_mesh.sondas, campo, ok);
  if (c == AURA_SONDA_SIN_LUGAR) {
    Serial.printf("[MESH] !! no se pueden seguir mas sondas (max %d, nombre < %d): %s\n",
                  AURA_SONDAS_MAX, AURA_CAMPO_MAX, campo);
    return;
  }
  if (c == AURA_SONDA_SIN_CAMBIO) return;
  JsonDocument det;
  det["campo"] = campo;
  det["motivo"] = c == AURA_SONDA_FALLO ? motivo_falla : "recuperada";
  char msg[64];
  snprintf(msg, sizeof(msg), c == AURA_SONDA_FALLO ? "Sonda %s con falla" : "Sonda %s recuperada", campo);
  nodo_mesh_alerta("sensor", c == AURA_SONDA_FALLO ? "high" : "info", msg, det.as<JsonObjectConst>());
}

// Cambio de alimentacion: alerta "energia" y reporte, solo si cambio.
static inline void nodo_mesh_alimentacion(const char* alimentacion) {
  if (nodo_mesh.alimentacion && strcmp(nodo_mesh.alimentacion, alimentacion) == 0) return;
  nodo_mesh.alimentacion = alimentacion;
  JsonDocument det;
  det["alimentacion"] = alimentacion;
  bool bateria = strcmp(alimentacion, "bateria") == 0;
  nodo_mesh_alerta("energia", bateria ? "warning" : "info",
                   bateria ? "El nodo paso a bateria" : "El nodo volvio a la red",
                   det.as<JsonObjectConst>());
  nodo_mesh_reportar();
}

// ===== Mediciones =====
// Guarda una medicion en la cola. values lleva SOLO mediciones validas: una
// sonda que falla no aparece (ni centinela ni null), y si no hay ninguna valida
// no se llama (contrato §3.1). false = no se guardo (vacia, muy grande o flash).
static inline bool nodo_mesh_medicion(JsonObjectConst values) {
  if (!nodo_mesh.cola_ok) {
    Serial.println("[MESH] !! la cola en flash no esta disponible, medicion perdida");
    return false;
  }
  if (values.isNull() || values.size() == 0) return false;
  size_t n = measureJson(values);
  if (n > AURA_VALUES_MAX) {
    Serial.printf("[MESH] !! values de %u bytes, el maximo es %u\n", (unsigned)n, AURA_VALUES_MAX);
    return false;
  }
  MuestraAura m;
  memset(&m, 0, sizeof(m));
  aura_ingest_id_nuevo(m.ingest_id, nodo_mesh_azar);
  m.ts = nodo_mesh_ahora();
  char json[AURA_VALUES_MAX + 1];
  serializeJson(values, json, sizeof(json));
  m.largo = (uint8_t)n;
  memcpy(m.values, json, n);
  if (!cola_push(&nodo_mesh.cola, &m)) {
    Serial.println("[MESH] !! no se pudo escribir la cola en flash");
    return false;
  }
  char id[37];
  aura_ingest_id_texto(m.ingest_id, id);
  Serial.printf("[MESH] medicion %s guardada (%s), en cola %lu\n", id, json,
                (unsigned long)cola_cantidad(&nodo_mesh.cola));
  return true;
}

static inline uint32_t nodo_mesh_pendientes() {
  return nodo_mesh.cola_ok ? cola_cantidad(&nodo_mesh.cola) : 0;
}

// ===== Comandos =====
static inline void nodo_mesh_resultado(const char* command_id, bool aplicado, const char* motivo) {
  JsonDocument d;
  if (command_id) d["command_id"] = command_id;
  d["aplicado"] = aplicado;
  if (motivo && motivo[0]) d["motivo"] = motivo;
  nodo_mesh_describir(d["config"].to<JsonObject>());
  if (measureJson(d) > AURA_PAYLOAD_MAX) d.remove("config");  // sin config antes que sin resultado
  nodo_mesh_encolar_evento(AURA_TIPO_RESULTADO, d);
}

static inline void nodo_mesh_comando(const RecibidaAura* r) {
  // Los comandos solo vienen del raiz: una retransmision trae el mismo seq.
  if (nodo_mesh.hay_ultimo_comando && nodo_mesh.ultimo_comando_seq == r->t.seq) return;
  nodo_mesh.hay_ultimo_comando = true;
  nodo_mesh.ultimo_comando_seq = r->t.seq;

  char json[AURA_PAYLOAD_MAX + 1];
  aura_payload_texto(&r->t, json, sizeof(json));
  Serial.printf("[MESH] comando: %s\n", json);

  JsonDocument d;
  if (deserializeJson(d, json)) { nodo_mesh_resultado(NULL, false, "json_invalido"); return; }
  const char* command = d["command"] | "";
  const char* command_id = d["command_id"];
  JsonObjectConst params = d["params"].as<JsonObjectConst>();
  String motivo;

  if (strcmp(command, "set_config") == 0) {
    if (!nodo_mesh.cb.aplicar_config) { nodo_mesh_resultado(command_id, false, "sin_parametros"); return; }
    bool ok = nodo_mesh.cb.aplicar_config(params, motivo);
    nodo_mesh_resultado(command_id, ok, ok ? "" : motivo.c_str());
    if (ok) nodo_mesh_reportar();
    return;
  }
  if (nodo_mesh.cb.ejecutar_comando) {
    bool ok = nodo_mesh.cb.ejecutar_comando(command, params, motivo);
    nodo_mesh_resultado(command_id, ok, ok ? "" : (motivo.length() ? motivo.c_str() : "comando_desconocido"));
    return;
  }
  nodo_mesh_resultado(command_id, false, "comando_desconocido");
}

static inline void nodo_mesh_confirmacion(const RecibidaAura* r) {
  uint8_t id[AURA_INGEST_ID_BYTES];
  uint32_t hora;
  if (!aura_confirmacion_leer(&r->t, id, &hora)) return;

  // La hora del raiz es la unica que tiene el nodo: con ella se completa
  // "ts". Se reajusta si se corrio mas de 2 s (el RTC deriva en meses de uso).
  uint32_t mia = nodo_mesh_ahora();
  if (aura_hora_valida(hora) && (mia == 0 || (mia > hora ? mia - hora : hora - mia) > 2)) {
    struct timeval tv = {(time_t)hora, 0};
    settimeofday(&tv, NULL);
    Serial.printf("[MESH] hora ajustada por el raiz: %lu\n", (unsigned long)hora);
  }

  // Solo se saca de la cola la muestra que se confirma. Una confirmacion
  // atrasada de otra muestra (un reintento) no toca nada.
  MuestraAura cabeza;
  if (!cola_peek(&nodo_mesh.cola, &cabeza) || !aura_ingest_id_igual(cabeza.ingest_id, id)) return;
  if (!cola_pop(&nodo_mesh.cola)) {
    Serial.println("[MESH] !! no se pudo actualizar la cola en flash");
    return;
  }
  nodo_mesh.en_vuelo = false;
  nodo_mesh.intentos = 0;
  nodo_mesh.proximo_envio = millis();
  nodo_mesh.confirmadas++;
  char t[37];
  aura_ingest_id_texto(id, t);
  Serial.printf("[MESH] AURA confirmo %s, quedan %lu\n", t, (unsigned long)cola_cantidad(&nodo_mesh.cola));
}

static inline void nodo_mesh_procesar_recibidas() {
  RecibidaAura r;
  while (radio_recibir(&r)) {
    if (r.t.tipo == AURA_TIPO_CONFIRMACION) nodo_mesh_confirmacion(&r);
    else if (r.t.tipo == AURA_TIPO_COMANDO) nodo_mesh_comando(&r);
  }
}

// ===== Envio =====
// true = la mesh acepto la trama hacia el raiz (con reintentos salto a salto).
// No es confirmacion de AURA: esa llega aparte, como CONFIRMACION.
static inline bool nodo_mesh_al_raiz(const TramaAura* t) { return radio_al_raiz(t); }

static inline void nodo_mesh_enviar_evento() {
  TramaAura t;
  if (!buffer_peek(&nodo_mesh.eventos, &t)) return;
  if (nodo_mesh_al_raiz(&t) || ++nodo_mesh.intentos_evento >= NODO_MESH_INTENTOS_EVENTO) {
    if (nodo_mesh.intentos_evento >= NODO_MESH_INTENTOS_EVENTO)
      Serial.printf("[MESH] !! evento tipo %u descartado: la mesh no lo acepto\n", t.tipo);
    buffer_pop(&nodo_mesh.eventos, &t);
    nodo_mesh.intentos_evento = 0;
  }
}

// Una sola muestra en vuelo: la mas vieja. Se reenvia si AURA no la confirma
// a tiempo, con espera creciente (15 s ... 5 min).
static inline void nodo_mesh_enviar_muestra() {
  if (!nodo_mesh.cola_ok || (long)(millis() - nodo_mesh.proximo_envio) < 0) return;
  MuestraAura m;
  if (!cola_peek(&nodo_mesh.cola, &m)) return;

  bool misma = nodo_mesh.en_vuelo && aura_ingest_id_igual(m.ingest_id, nodo_mesh.id_en_vuelo);
  if (misma && millis() - nodo_mesh.enviada_en < aura_espera_confirmacion_ms(nodo_mesh.intentos)) return;
  if (misma) nodo_mesh.intentos++;
  else nodo_mesh.intentos = 0;

  TramaAura t;
  aura_telemetria_armar(&t, nodo_mesh.seq++, &m);
  if (nodo_mesh_al_raiz(&t)) {
    nodo_mesh.en_vuelo = true;
    memcpy(nodo_mesh.id_en_vuelo, m.ingest_id, AURA_INGEST_ID_BYTES);
    nodo_mesh.enviada_en = millis();
  } else {
    nodo_mesh.en_vuelo = false;
    nodo_mesh.proximo_envio = millis() + NODO_MESH_REINTENTO_SALTO_MS;
    Serial.println("[MESH] la mesh no acepto la muestra, reintento");
  }
}

// ===== API =====
static inline bool nodo_mesh_iniciar(NodoMeshCallbacks cb) {
  static const uint8_t mesh_id[6]  = MESH_ID;
  static const uint8_t esperada[6] = MAC_ESPERADA;

  memset(&nodo_mesh, 0, sizeof(nodo_mesh));
  nodo_mesh.cb = cb;
  nodo_mesh.alimentacion = cb.alimentacion ? cb.alimentacion : "desconocida";
  nodo_mesh.configurada = !aura_mac_vacia(mesh_id);
  buffer_init(&nodo_mesh.eventos);
  aura_sondas_init(&nodo_mesh.sondas);

  AlmacenAura alm = {nodo_mesh_fs_leer, nodo_mesh_fs_escribir, NULL};
  nodo_mesh.cola_ok = nodo_mesh_fs_preparar() && cola_abrir(&nodo_mesh.cola, alm, AURA_COLA_CAP);
  if (!nodo_mesh.cola_ok) Serial.println("[MESH] !! no se pudo abrir la cola en flash");
  else if (nodo_mesh.cola.formateada) Serial.println("[MESH] cola nueva (no habia una valida en flash)");

  if (!nodo_mesh.configurada) {
    // Sin mesh se puede igual desarrollar la medicion: se guarda en la cola.
    Serial.println("[MESH] AVISO: MESH_ID sin configurar (config_local.h): se mide y se guarda, no se envia");
    return nodo_mesh.cola_ok;
  }

  AuraMeshConfig mc = {{0}, MESH_CLAVE, MESH_CANAL, MESH_ROUTER_SSID, ""};
  memcpy(mc.mesh_id, mesh_id, 6);
  if (!radio_iniciar(AURA_ROL_HOJA, &mc, 8)) {
    Serial.println("[MESH] !! no arranco la mesh, reinicio");
    delay(2000);
    ESP.restart();
  }
  radio_verificar_placa(esperada);

  const uint8_t* yo = radio_mi_mac();
  Serial.printf("[MESH] hoja lista, hw_id mac-%02x%02x%02x%02x%02x%02x, pendientes de confirmar %lu\n",
                yo[0], yo[1], yo[2], yo[3], yo[4], yo[5], (unsigned long)nodo_mesh_pendientes());
  // El reporte sale cuando la hoja se une a la mesh (radio_reconecto).
  return nodo_mesh.cola_ok;
}

static inline void nodo_mesh_loop() {
  if (!nodo_mesh.configurada) return;
  nodo_mesh_procesar_recibidas();
  if (!radio_conectada()) return;   // la cola guarda; la mesh se reengancha sola

  // Al unirse o volver a la mesh se reporta: sin el reporte, el raiz no conoce
  // el intervalo y no puede inferir offline.
  if (radio_reconecto()) {
    Serial.printf("[MESH] unida a la mesh, capa %d, tipo %s\n", radio_capa(),
                  esp_mesh_get_type() == MESH_LEAF ? "hoja" : "NO ES HOJA");
    nodo_mesh_reportar();
  }

  nodo_mesh_enviar_evento();
  nodo_mesh_procesar_recibidas();   // un comando no espera a la proxima vuelta
  nodo_mesh_enviar_muestra();
}
