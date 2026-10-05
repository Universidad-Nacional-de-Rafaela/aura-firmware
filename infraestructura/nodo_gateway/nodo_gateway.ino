/*
 * AURA - gateway de la mesh (XIAO ESP32S3), contrato v3.0
 *
 * Unico nodo de la mesh que conoce AURA: traduce MAC <-> device_id y es el
 * adaptador de la mesh en el sentido del contrato (docs/CONTRATO_MQTT.md).
 *
 *   telemetria  -> POST /api/v1/telemetry/ingest, UN evento por request (§6).
 *                  Solo si AURA dice que la persistio, le manda al nodo la
 *                  CONFIRMACION, y recien ahi el nodo la saca de su cola.
 *                  El gateway no guarda muestras: si se cae, el nodo reintenta.
 *   reporte     -> devices/<id>/status, retain, con details de la mesh (§3.2)
 *   resultado   -> devices/<id>/response: recibido + aplicado / rechazado (§3.4)
 *   alerta      -> alerts/<id>/<tipo> (§3.5)
 *   comandos    <- devices/<id>/command, validados y bajados por ESP-NOW (§3.3)
 *
 * Ademas infiere offline (3 x intervalo sin tramas) y publica su propio status
 * con LWT. La telemetria NUNCA va por MQTT (la v1.x lo hacia).
 *
 * Configuracion (red, UUID, tabla de nodos) en config_local.h, que no se
 * versiona: ver config_local.h.example. Sin el, compila pero no hace nada util.
 *
 * Banco sin backend: herramientas/ingesta_falsa.py hace de API.
 *
 * IDE: Placa "XIAO_ESP32S3". USB CDC On Boot: ENABLED.
 * Bibliotecas: las de bibliotecas.txt.
 */

#if __has_include("config_local.h")
#include "config_local.h"
#endif

#include <HTTPClient.h>
#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "../../comun/radio_mesh.h"
#include "../../comun/ingest_id.h"
#include "gateway_logica.h"

// ===== CONFIGURACION (valores por defecto: sin config_local.h no conecta) =====
#ifndef WIFI_SSID
#define WIFI_SSID "CAMBIAR"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "CAMBIAR"
#endif
#ifndef MQTT_HOST
#define MQTT_HOST "CAMBIAR"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef API_BASE
#define API_BASE "http://CAMBIAR:8000"
#endif
#ifndef TENANT_ID
#define TENANT_ID "00000000-0000-0000-0000-000000000000"
#endif
#ifndef GATEWAY_DEVICE_ID
#define GATEWAY_DEVICE_ID "00000000-0000-0000-0000-000000000000"
#endif
#ifndef NTP_SERVIDOR
#define NTP_SERVIDOR "pool.ntp.org"
#endif
#ifndef MAC_ESPERADA
#define MAC_ESPERADA {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#endif

// Tabla MAC -> device_id (contrato §2.4). Una MAC que no esta aca se descarta y
// suma a "huerfanos". padre en cero = el nodo le llega directo al gateway.
// comandos: los que acepta ese nodo, separados por coma ("" = ninguno).
typedef struct {
  uint8_t     mac[6];
  const char* device_id;
  const char* tipo;       // "type" del evento REST
  uint8_t     padre[6];
  const char* comandos;
} NodoConocido;

#ifndef TABLA_NODOS
#define TABLA_NODOS { { {0,0,0,0,0,0}, "", "", {0,0,0,0,0,0}, "" } }
#endif

const NodoConocido TABLA[] = TABLA_NODOS;
const int TABLA_N = sizeof(TABLA) / sizeof(TABLA[0]);
const uint8_t mac_esperada[6] = MAC_ESPERADA;

const unsigned long INTERVALO_STATUS   = 60000;
const unsigned long ESPERA_RESULTADO   = 30000;   // sin resultado del nodo -> rechazado
const uint16_t      HTTP_TIMEOUT_MS    = 2500;    // menor que el timeout de ACK de la sala

// ===== ESTADO =====
WiFiClient net;
MqttClient mqtt(net);
WiFiClient net_http;   // propio: con el de MQTT, cada POST cortaria la sesion con el broker

typedef struct {
  bool          visto;
  unsigned long ultimo_ms;
  uint32_t      intervalo_s;  // del ultimo reporte; 0 = desconocido, no se infiere offline
  bool          offline;
  int8_t        rssi;
  bool          tiene_seq;
  uint16_t      ultima_seq;   // de resultado/reporte/alerta: una retransmision no se publica dos veces
  String        details;      // del ultimo reporte, para volver a online con ellos
} EstadoNodo;

EstadoNodo estado[TABLA_N];

// Tramas recibidas, ya confirmadas al salto anterior, esperando proceso.
#define PENDIENTES_CAP 32
RecibidaAura pendientes[PENDIENTES_CAP];
uint8_t pend_inicio = 0, pend_n = 0;

// Comandos que llegaron por MQTT, para procesar en el loop.
typedef struct { int idx; String msg; } ComandoRecibido;
#define COMANDOS_CAP 4
ComandoRecibido comandos[COMANDOS_CAP];
uint8_t cmd_n = 0;

// Comandos transmitidos esperando el resultado del nodo.
typedef struct { bool activo; int idx; String command_id; unsigned long desde; } ComandoEnCurso;
#define EN_CURSO_CAP 8
ComandoEnCurso en_curso[EN_CURSO_CAP];

uint16_t seq_actual = 0;
uint32_t persistidos = 0, duplicados = 0, rest_fallidos = 0, values_invalidos = 0;
uint32_t huerfanos = 0, ajenas = 0, descartadas_gw = 0, repetidas = 0;
int      ultimo_http = 0;

// ===== UTILIDADES =====
int buscar(const uint8_t mac[6]) {
  for (int i = 0; i < TABLA_N; i++)
    if (!aura_mac_vacia(TABLA[i].mac) && memcmp(TABLA[i].mac, mac, 6) == 0) return i;
  return -1;
}

// Proximo salto hacia un nodo: su padre, o el nodo mismo si cuelga del gateway.
const uint8_t* siguiente_salto(int idx) {
  return aura_mac_vacia(TABLA[idx].padre) ? TABLA[idx].mac : TABLA[idx].padre;
}

uint32_t ahora_epoca() {
  time_t t = time(NULL);
  return (t > 0 && aura_hora_valida((uint32_t)t)) ? (uint32_t)t : 0;
}

void publicar(const String& topico, const String& cuerpo, bool retain) {
  if (!mqtt.connected()) {
    Serial.printf("[MQTT] !! sin broker, no se publica %s\n", topico.c_str());
    return;
  }
  // Con el largo conocido de antemano no se usa el buffer de 256 B de la biblioteca.
  mqtt.beginMessage(topico, (unsigned long)cuerpo.length(), retain, 1);
  mqtt.print(cuerpo);
  mqtt.endMessage();
  Serial.printf("[MQTT] TX %s %s\n", topico.c_str(), cuerpo.c_str());
}

void publicar_doc(const String& topico, JsonDocument& doc, bool retain) {
  String cuerpo;
  serializeJson(doc, cuerpo);
  publicar(topico, cuerpo, retain);
}

String topico(const char* base, int idx, const char* sufijo) {
  return String(base) + TABLA[idx].device_id + sufijo;
}

// ===== RESPUESTAS (contrato §3.4) =====
void publicar_respuesta(int idx, const char* status, const char* command_id,
                        const char* motivo, JsonVariantConst config) {
  JsonDocument d;
  d["status"] = status;
  JsonObject det = d["details"].to<JsonObject>();
  if (command_id && command_id[0]) det["command_id"] = command_id;
  if (motivo && motivo[0]) det["motivo"] = motivo;
  if (!config.isNull()) det["config"] = config;
  publicar_doc(topico("devices/", idx, "/response"), d, false);
}

void quitar_en_curso(int idx, const char* command_id) {
  for (int i = 0; i < EN_CURSO_CAP; i++)
    if (en_curso[i].activo && en_curso[i].idx == idx &&
        en_curso[i].command_id == (command_id ? command_id : ""))
      en_curso[i].activo = false;
}

// ===== RECEPCION =====
// Se vacia la cola de la radio en cada vuelta: el ACK al salto anterior sale
// enseguida, aunque despues el POST tarde (lecciones A y E de mesh-v1).
void drenar_radio() {
  RecibidaAura r;
  while (radio_recibir(&r)) {
    if (!aura_es_para_mi(&r.t, radio_mi_mac())) { ajenas++; continue; }
    radio_mandar_ack(r.de, r.t.seq);
    if (!aura_tipo_sube(r.t.tipo)) continue;   // PING: con el ACK alcanza

    if (pend_n == PENDIENTES_CAP) {            // se pierde la mas vieja; su nodo reintenta
      pend_inicio = (pend_inicio + 1) % PENDIENTES_CAP;
      pend_n--;
      descartadas_gw++;
    }
    pendientes[(pend_inicio + pend_n) % PENDIENTES_CAP] = r;
    pend_n++;
  }
}

// ===== TELEMETRIA -> REST (contrato §6) =====
void subir_telemetria(int idx, const RecibidaAura* r) {
  MuestraAura m;
  if (!aura_telemetria_leer(&r->t, &m)) { values_invalidos++; return; }
  char id[37];
  aura_ingest_id_texto(m.ingest_id, id);

  TramaAura conf;
  aura_confirmacion_armar(&conf, radio_mi_mac(), TABLA[idx].mac, seq_actual++, m.ingest_id, ahora_epoca());

  JsonDocument values;
  if (deserializeJson(values, m.values, m.largo) || !values.is<JsonObject>() || values.size() == 0) {
    // Nunca va a persistir: se confirma igual para que no trabe la cola del nodo.
    values_invalidos++;
    Serial.printf("[REST] !! values invalido en %s, se descarta y se confirma\n", id);
    radio_enviar(siguiente_salto(idx), &conf);
    return;
  }

  JsonDocument cuerpo;
  JsonObject ev = cuerpo["events"].add<JsonObject>();
  ev["tenant_id"] = TENANT_ID;
  ev["device_id"] = TABLA[idx].device_id;
  ev["type"]      = TABLA[idx].tipo;
  char iso[GW_ISO_BYTES];
  if (gw_epoca_a_iso(m.ts, iso)) ev["ts"] = iso;   // sin hora valida, se omite
  ev["ingest_id"] = id;
  ev["payload"]   = values.as<JsonObjectConst>();
  String json;
  serializeJson(cuerpo, json);

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(net_http, String(API_BASE) + "/api/v1/telemetry/ingest");
  http.addHeader("Content-Type", "application/json");
#ifdef API_TOKEN
  http.addHeader("Authorization", String("Bearer ") + API_TOKEN);
#endif
  int codigo = http.POST(json);
  String respuesta = codigo > 0 ? http.getString() : String();
  http.end();
  ultimo_http = codigo;

  JsonDocument resp;
  long ins = -1, dup = -1, err = -1;
  if (!deserializeJson(resp, respuesta)) {
    ins = resp["inserted"] | -1L;
    dup = resp["duplicates"] | -1L;
    err = resp["errors"] | -1L;
  }

  if (gw_rest_persistio(codigo, ins, dup, err, 1)) {
    if (dup > 0) duplicados++; else persistidos++;
    radio_enviar(siguiente_salto(idx), &conf);
    Serial.printf("[REST] OK %s %s -> confirmado al nodo\n", id, dup > 0 ? "(duplicado)" : "");
  } else {
    rest_fallidos++;
    Serial.printf("[REST] !! %s no persistio (HTTP %d: %s). El nodo reintenta.\n",
                  id, codigo, respuesta.c_str());
  }
}

// ===== RESULTADO, REPORTE, ALERTA =====
void publicar_resultado(int idx, JsonDocument& d) {
  const char* command_id = d["command_id"] | "";
  bool aplicado = d["aplicado"] | false;
  quitar_en_curso(idx, command_id);
  publicar_respuesta(idx, "recibido", command_id, NULL, JsonVariantConst());
  if (aplicado) publicar_respuesta(idx, "aplicado", command_id, NULL, d["config"]);
  else publicar_respuesta(idx, "rechazado", command_id, d["motivo"] | "rechazado_por_el_nodo", JsonVariantConst());
}

void publicar_reporte(int idx, JsonDocument& d, int8_t rssi) {
  JsonDocument s;
  s["status"] = "online";
  JsonObject det = s["details"].to<JsonObject>();
  det["evento"]       = "reporte";
  det["transporte"]   = "mesh";
  det["config"]       = d["config"];
  det["pendientes"]   = d["pendientes"];
  det["descartadas"]  = d["descartadas"];
  det["alimentacion"] = d["alimentacion"] | "desconocida";
  det["rssi"]         = rssi;
  estado[idx].intervalo_s = d["config"]["intervalo_s"] | 0u;
  estado[idx].details = "";
  serializeJson(det, estado[idx].details);
  publicar_doc(topico("devices/", idx, "/status"), s, true);
}

// El tipo va en el topico: solo [a-z_], para que no meta niveles ni comodines.
bool tipo_alerta_valido(const char* t) {
  if (!t || !t[0] || strlen(t) > 24) return false;
  for (const char* p = t; *p; p++) if (!((*p >= 'a' && *p <= 'z') || *p == '_')) return false;
  return true;
}

void publicar_alerta(int idx, JsonDocument& d) {
  const char* tipo = d["tipo"] | "";
  if (!tipo_alerta_valido(tipo)) {
    Serial.printf("[ALERTA] !! tipo invalido de %s, se descarta\n", TABLA[idx].device_id);
    return;
  }
  JsonDocument a;
  a["severity"] = d["severity"] | "unknown";
  if (!d["message"].isNull()) a["message"] = d["message"];
  if (!d["details"].isNull()) a["details"] = d["details"];
  char iso[GW_ISO_BYTES];
  if (gw_epoca_a_iso(ahora_epoca(), iso)) a["ts"] = iso;
  publicar_doc(topico("alerts/", idx, (String("/") + tipo).c_str()), a, false);
}

// Cualquier trama de un nodo prueba que esta vivo.
void marcar_visto(int idx, int8_t rssi) {
  EstadoNodo& e = estado[idx];
  e.visto = true;
  e.ultimo_ms = millis();
  e.rssi = rssi;
  if (e.offline) {
    e.offline = false;
    String s = "{\"status\":\"online\",\"details\":" +
               (e.details.length() ? e.details : String("{\"transporte\":\"mesh\"}")) + "}";
    publicar(topico("devices/", idx, "/status"), s, true);
  }
}

void procesar_una() {
  if (pend_n == 0) return;
  RecibidaAura r = pendientes[pend_inicio];
  pend_inicio = (pend_inicio + 1) % PENDIENTES_CAP;
  pend_n--;

  int idx = buscar(r.t.mac_origen);
  char m[18];
  if (idx < 0) {
    huerfanos++;
    Serial.printf("[MESH] MAC %s no esta en la tabla, se descarta\n", aura_mac_texto(r.t.mac_origen, m));
    return;
  }
  marcar_visto(idx, r.rssi);

  if (r.t.tipo == AURA_TIPO_TELEMETRIA) { subir_telemetria(idx, &r); return; }

  // Resultado, reporte o alerta: una retransmision de la sala no se publica dos veces.
  EstadoNodo& e = estado[idx];
  if (e.tiene_seq && e.ultima_seq == r.t.seq) { repetidas++; return; }
  e.tiene_seq = true;
  e.ultima_seq = r.t.seq;

  char json[AURA_PAYLOAD_MAX + 1];
  aura_payload_texto(&r.t, json, sizeof(json));
  JsonDocument d;
  if (deserializeJson(d, json) || !d.is<JsonObject>()) {
    Serial.printf("[MESH] !! JSON invalido de %s: %s\n", TABLA[idx].device_id, json);
    return;
  }
  if (r.t.tipo == AURA_TIPO_RESULTADO) publicar_resultado(idx, d);
  else if (r.t.tipo == AURA_TIPO_REPORTE) publicar_reporte(idx, d, r.rssi);
  else if (r.t.tipo == AURA_TIPO_ALERTA) publicar_alerta(idx, d);
}

// ===== COMANDOS (contrato §3.3) =====
// Corre dentro de mqtt.poll(): solo copia; se procesa en el loop.
void on_mqtt(int size) {
  String t = mqtt.messageTopic();
  String msg;
  msg.reserve(size);
  while (mqtt.available()) msg += (char)mqtt.read();
  for (int i = 0; i < TABLA_N; i++) {
    if (aura_mac_vacia(TABLA[i].mac) || t != topico("devices/", i, "/command")) continue;
    if (cmd_n == COMANDOS_CAP) {
      publicar_respuesta(i, "rechazado", NULL, "gateway_ocupado", JsonVariantConst());
      return;
    }
    comandos[cmd_n].idx = i;
    comandos[cmd_n].msg = msg;
    cmd_n++;
    return;
  }
}

void procesar_comando(int idx, const String& msg) {
  Serial.printf("[BAJA] RX %s %s\n", TABLA[idx].device_id, msg.c_str());
  JsonDocument d;
  if (deserializeJson(d, msg) || !d.is<JsonObject>()) {
    publicar_respuesta(idx, "rechazado", NULL, "json_invalido", JsonVariantConst());
    return;
  }
  const char* command_id = d["command_id"] | "";
  const char* command = d["command"] | "";
  if (!command[0]) {
    publicar_respuesta(idx, "rechazado", command_id, "sin_command", JsonVariantConst());
    return;
  }
  if (!gw_comando_permitido(TABLA[idx].comandos, command)) {
    publicar_respuesta(idx, "rechazado", command_id, "comando_no_soportado", JsonVariantConst());
    return;
  }
  // El JSON se baja compactado: el limite es el payload de la trama (180 B).
  char json[AURA_PAYLOAD_MAX + 1];
  size_t n = measureJson(d);
  if (n > AURA_PAYLOAD_MAX) {
    publicar_respuesta(idx, "rechazado", command_id, "excede_180_bytes", JsonVariantConst());
    return;
  }
  serializeJson(d, json, sizeof(json));

  TramaAura t;
  aura_trama_init(&t, AURA_TIPO_COMANDO, radio_mi_mac(), TABLA[idx].mac, seq_actual++,
                  (const uint8_t*)json, (uint8_t)n);
  if (!radio_enviar(siguiente_salto(idx), &t)) {
    publicar_respuesta(idx, "rechazado", command_id, "radio_no_acepto", JsonVariantConst());
    return;
  }
  publicar_respuesta(idx, "transmitido", command_id, NULL, JsonVariantConst());
  for (int i = 0; i < EN_CURSO_CAP; i++) {
    if (en_curso[i].activo) continue;
    en_curso[i] = {true, idx, String(command_id), millis()};
    break;
  }
}

// Un comando transmitido sin resultado del nodo no queda sin respuesta.
void vencer_comandos() {
  for (int i = 0; i < EN_CURSO_CAP; i++) {
    if (!en_curso[i].activo || millis() - en_curso[i].desde < ESPERA_RESULTADO) continue;
    en_curso[i].activo = false;
    publicar_respuesta(en_curso[i].idx, "rechazado", en_curso[i].command_id.c_str(),
                       "sin_resultado_del_nodo", JsonVariantConst());
  }
}

// ===== OFFLINE INFERIDO (contrato §3.2) =====
void revisar_offline() {
  for (int i = 0; i < TABLA_N; i++) {
    EstadoNodo& e = estado[i];
    if (!e.visto || !gw_debe_marcar_offline(millis(), e.ultimo_ms, e.intervalo_s, e.offline)) continue;
    e.offline = true;
    publicar(topico("devices/", i, "/status"),
             "{\"status\":\"offline\",\"details\":{\"motivo\":\"sin_uplinks\",\"transporte\":\"mesh\"}}", true);
  }
}

// ===== STATUS PROPIO =====
void publicar_status_gateway() {
  uint8_t canal;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&canal, &sec);
  char m[18];
  JsonDocument d;
  d["status"] = "online";
  JsonObject det = d["details"].to<JsonObject>();
  det["transporte"]       = "mesh";
  det["rol"]              = "gateway";
  det["mac"]              = aura_mac_texto(radio_mi_mac(), m);
  det["canal"]            = canal;
  det["rssi_wifi"]        = WiFi.RSSI();
  det["persistidos"]      = persistidos;
  det["duplicados"]       = duplicados;
  det["rest_fallidos"]    = rest_fallidos;
  det["ultimo_http"]      = ultimo_http;
  det["values_invalidos"] = values_invalidos;
  det["huerfanos"]        = huerfanos;
  det["ajenas"]           = ajenas;
  det["repetidas"]        = repetidas;
  det["descartadas"]      = descartadas_gw;
  det["version_distinta"] = radio_aura.version_distinta;
  det["invalidas"]        = radio_aura.invalidas;
  det["desbordes_radio"]  = radio_aura.desbordes;
  det["hora_valida"]      = ahora_epoca() != 0;
  det["uptime_s"]         = millis() / 1000;
  publicar_doc(String("devices/") + GATEWAY_DEVICE_ID + "/status", d, true);
}

// ===== CONEXION =====
bool conectar_mqtt() {
  // Last Will: si esta placa muere, el broker publica offline por ella.
  String top = String("devices/") + GATEWAY_DEVICE_ID + "/status";
  const char* lwt = "{\"status\":\"offline\"}";
  mqtt.beginWill(top, strlen(lwt), true, 1);
  mqtt.print(lwt);
  mqtt.endWill();

  if (!mqtt.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.printf("[MQTT] !! fallo la conexion, error %d. Con WiFi conectado, lo mas probable\n"
                  "       es aislamiento de clientes en la red de invitados.\n", mqtt.connectError());
    return false;
  }
  for (int i = 0; i < TABLA_N; i++) {
    if (aura_mac_vacia(TABLA[i].mac)) continue;
    mqtt.subscribe(topico("devices/", i, "/command"), 1);
  }
  Serial.println("[MQTT] conectado y suscripto a los comandos de la tabla");
  publicar_status_gateway();
  return true;
}

void mantener_conexiones() {
  static unsigned long ultimo_intento = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - ultimo_intento > 10000) {
      ultimo_intento = millis();
      Serial.println("[WIFI] caido, reconectando...");
      WiFi.reconnect();
    }
    return;
  }
  if (!mqtt.connected() && millis() - ultimo_intento > 5000) {
    ultimo_intento = millis();
    conectar_mqtt();
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("conectando a %s", WIFI_SSID);
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  WiFi.setSleep(false);   // sin esto se pierden tramas ESP-NOW

  if (!radio_iniciar(16)) {
    Serial.println("\nfallo ESP-NOW, reinicio");
    delay(2000);
    ESP.restart();
  }
  radio_verificar_placa(mac_esperada);
  for (int i = 0; i < TABLA_N; i++) {
    radio_agregar_peer(TABLA[i].mac);
    radio_agregar_peer(TABLA[i].padre);
  }

  uint8_t canal;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&canal, &sec);
  Serial.printf("\nWiFi %s  IP %s  RSSI %d\n", WiFi.status() == WL_CONNECTED ? "ok" : "SIN CONEXION",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  Serial.printf("CANAL DEL AP = %u  <- la mesh tiene que estar aca\n", canal);

  // La hora solo se usa para "ts" de las alertas y para dar hora a los nodos.
  configTime(0, 0, NTP_SERVIDOR);

  mqtt.onMessage(on_mqtt);
  conectar_mqtt();

  int validos = 0;
  for (int i = 0; i < TABLA_N; i++) if (!aura_mac_vacia(TABLA[i].mac)) validos++;
  Serial.printf("nodo_gateway listo: %d nodo(s) en la tabla\n", validos);
  if (validos == 0) Serial.println("AVISO: tabla vacia, falta config_local.h");
}

// ===== LOOP =====
void loop() {
  drenar_radio();           // primero confirmar: la mesh espera esto
  mantener_conexiones();
  mqtt.poll();

  if (cmd_n > 0) {          // lo que baja antes: un comando espera a una persona
    ComandoRecibido c = comandos[0];
    for (int i = 1; i < cmd_n; i++) comandos[i - 1] = comandos[i];
    cmd_n--;
    procesar_comando(c.idx, c.msg);
  }
  drenar_radio();
  if (WiFi.status() == WL_CONNECTED) procesar_una();
  drenar_radio();

  static unsigned long ultimo_chequeo = 0;
  if (millis() - ultimo_chequeo > 5000) {
    ultimo_chequeo = millis();
    revisar_offline();
    vencer_comandos();
  }

  static unsigned long ultimo_status = 0;
  if (millis() - ultimo_status > INTERVALO_STATUS) {
    ultimo_status = millis();
    publicar_status_gateway();
  }

  delay(5);
}
