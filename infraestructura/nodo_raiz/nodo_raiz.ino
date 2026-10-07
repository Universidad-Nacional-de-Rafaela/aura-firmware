/*
 * AURA - raiz de la mesh (XIAO ESP32S3), contrato v4.0, ESP-WIFI-MESH
 *
 * Unico nodo de la mesh que tiene IP: se asocia al WiFi del edificio y es el
 * adaptador de la mesh en el sentido del contrato (docs/CONTRATO_MQTT.md).
 * Publica con el hw_id de cada placa ("mac-..."): no tiene tabla de nodos ni
 * conoce UUID. AURA traduce el hw_id a su dispositivo, o lo pone en cuarentena.
 *
 *   TELEMETRIA  -> hw/<hoja>/data (values, ingest_id, ts, adaptador)
 *   ack         <- hw/<hoja>/ack: con un resultado valido, CONFIRMACION a la
 *                  hoja, que recien ahi saca la muestra de su cola. El raiz no
 *                  guarda muestras: si se cae, la hoja reintenta.
 *   REPORTE     -> hw/<hoja>/status, retain, con details de la mesh (§3.2)
 *   RESULTADO   -> hw/<hoja>/response: recibido + aplicado / rechazado (§3.4)
 *   ALERTA      -> hw/<hoja>/alerts/<tipo> (§3.5)
 *   comandos    <- hw/<hoja>/command, bajados por la mesh (§3.3)
 *
 * Ademas infiere offline (3 x intervalo sin tramas) y publica su propio status
 * en hw/<raiz>/status con LWT.
 *
 * Reemplaza a nodo_gateway (mesh ESP-NOW, tag mesh-espnow-v2).
 * Configuracion en config_local.h, que no se versiona (ver config_local.h.example).
 * Banco sin backend: herramientas/ack_falso.py hace de AURA.
 *
 * IDE: Placa "XIAO_ESP32S3". USB CDC On Boot: ENABLED.
 * Bibliotecas: las de bibliotecas.txt.
 */

#if __has_include("config_local.h")
#include "config_local.h"
#endif

#include <ArduinoMqttClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "../../comun/radio_wifi_mesh.h"
#include "../../comun/ingest_id.h"
#include "raiz_logica.h"

// ===== CONFIGURACION (valores por defecto: sin config_local.h no conecta) =====
#ifndef MESH_ID
#define MESH_ID {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#endif
#ifndef MESH_CLAVE
#define MESH_CLAVE ""
#endif
#ifndef MESH_CANAL
#define MESH_CANAL 0
#endif
#ifndef ROUTER_SSID
#define ROUTER_SSID "CAMBIAR"
#endif
#ifndef ROUTER_CLAVE
#define ROUTER_CLAVE "CAMBIAR"
#endif
#ifndef MQTT_HOST
#define MQTT_HOST "CAMBIAR"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef NTP_SERVIDOR
#define NTP_SERVIDOR "pool.ntp.org"
#endif
#ifndef MAC_ESPERADA
#define MAC_ESPERADA {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#endif

const uint8_t mesh_id[6]      = MESH_ID;
const uint8_t mac_esperada[6] = MAC_ESPERADA;

const unsigned long INTERVALO_STATUS = 60000;
const unsigned long ESPERA_RESULTADO = 30000;   // sin resultado de la hoja -> rechazado

// ===== ESTADO =====
WiFiClient net;
MqttClient mqtt(net);
char mi_hw[RAIZ_HW_ID_BYTES];

// Lo que el raiz sabe de cada hoja que le hablo. Sin esto no hay offline
// inferido ni descarte de repetidos, pero los datos se publican igual.
#define NODOS_CAP 64
typedef struct {
  bool          usado;
  uint8_t       mac[6];
  unsigned long ultimo_ms;
  uint32_t      intervalo_s;  // del ultimo reporte; 0 = desconocido, no se infiere offline
  bool          offline;
  bool          tiene_seq;
  uint16_t      ultima_seq;   // de resultado/reporte/alerta: un repetido no se publica dos veces
  String        details;      // del ultimo reporte, para volver a online con ellos
} EstadoNodo;
EstadoNodo nodos[NODOS_CAP];

// Comandos transmitidos esperando el resultado de la hoja.
#define EN_CURSO_CAP 8
typedef struct { bool activo; uint8_t mac[6]; String command_id; unsigned long desde; } ComandoEnCurso;
ComandoEnCurso en_curso[EN_CURSO_CAP];

uint16_t seq_actual = 0;
uint32_t publicadas = 0, confirmadas = 0, acks_ignorados = 0, ajenos = 0;
uint32_t values_invalidos = 0, repetidas = 0, sin_lugar = 0;

// ===== UTILIDADES =====
uint32_t ahora_epoca() {
  time_t t = time(NULL);
  return (t > 0 && aura_hora_valida((uint32_t)t)) ? (uint32_t)t : 0;
}

EstadoNodo* nodo(const uint8_t mac[6], bool crear) {
  EstadoNodo* libre = NULL;
  for (int i = 0; i < NODOS_CAP; i++) {
    if (nodos[i].usado && memcmp(nodos[i].mac, mac, 6) == 0) return &nodos[i];
    if (!nodos[i].usado && !libre) libre = &nodos[i];
  }
  if (!crear) return NULL;
  if (!libre) { sin_lugar++; return NULL; }
  libre->usado = true;
  memcpy(libre->mac, mac, 6);
  return libre;
}

String topico(const uint8_t mac[6], const char* sufijo) {
  char hw[RAIZ_HW_ID_BYTES], t[RAIZ_TOPICO_MAX];
  raiz_hw_id_de_mac(mac, hw);
  return raiz_topico(t, sizeof(t), hw, sufijo) ? String(t) : String();
}

void publicar(const String& top, const String& cuerpo, bool retain) {
  if (top.length() == 0) return;
  if (!mqtt.connected()) {
    Serial.printf("[MQTT] !! sin broker, no se publica %s\n", top.c_str());
    return;
  }
  // Con el largo conocido de antemano no se usa el buffer de 256 B de la biblioteca.
  mqtt.beginMessage(top, (unsigned long)cuerpo.length(), retain, 1);
  mqtt.print(cuerpo);
  mqtt.endMessage();
  publicadas++;
  Serial.printf("[MQTT] TX %s %s\n", top.c_str(), cuerpo.c_str());
}

void publicar_doc(const String& top, JsonDocument& doc, bool retain) {
  String cuerpo;
  serializeJson(doc, cuerpo);
  publicar(top, cuerpo, retain);
}

// ===== RESPUESTAS (contrato §3.4) =====
void publicar_respuesta(const uint8_t mac[6], const char* status, const char* command_id,
                        const char* motivo, JsonVariantConst config) {
  JsonDocument d;
  d["status"] = status;
  JsonObject det = d["details"].to<JsonObject>();
  if (command_id && command_id[0]) det["command_id"] = command_id;
  if (motivo && motivo[0]) det["motivo"] = motivo;
  if (!config.isNull()) det["config"] = config;
  publicar_doc(topico(mac, "response"), d, false);
}

void quitar_en_curso(const uint8_t mac[6], const char* command_id) {
  for (int i = 0; i < EN_CURSO_CAP; i++)
    if (en_curso[i].activo && memcmp(en_curso[i].mac, mac, 6) == 0 &&
        en_curso[i].command_id == (command_id ? command_id : ""))
      en_curso[i].activo = false;
}

// ===== SUBIDA: lo que manda una hoja =====
void subir_telemetria(const uint8_t mac[6], const TramaAura* t) {
  MuestraAura m;
  if (!aura_telemetria_leer(t, &m)) { values_invalidos++; return; }
  char id[37];
  aura_ingest_id_texto(m.ingest_id, id);
  char texto[AURA_VALUES_MAX + 1];
  memcpy(texto, m.values, m.largo);
  texto[m.largo] = '\0';

  JsonDocument d;
  JsonDocument values;
  // values tiene que ser un objeto JSON. Si la hoja manda otra cosa, se publica
  // como texto: AURA lo rechaza con un ack "rechazado" y la hoja libera su cola.
  // Descartarlo aca dejaria la muestra trabada para siempre en la hoja.
  if (deserializeJson(values, texto) == DeserializationError::Ok && values.is<JsonObject>()) {
    d["values"] = values.as<JsonObject>();
  } else {
    values_invalidos++;
    d["values"] = texto;
  }
  d["ingest_id"] = id;
  char iso[GW_ISO_BYTES];
  if (gw_epoca_a_iso(m.ts, iso)) d["ts"] = iso;   // sin hora valida, sin ts
  d["adaptador"] = mi_hw;
  publicar_doc(topico(mac, "data"), d, false);
}

void publicar_reporte(const uint8_t mac[6], EstadoNodo* n, JsonDocument& r) {
  JsonDocument d;
  d["status"] = "online";
  JsonObject det = d["details"].to<JsonObject>();
  det["evento"]       = "reporte";
  det["transporte"]   = "mesh";
  det["config"]       = r["config"];
  det["pendientes"]   = r["pendientes"];
  det["descartadas"]  = r["descartadas"];
  det["alimentacion"] = r["alimentacion"] | "desconocida";
  if (n) {
    n->intervalo_s = r["config"]["intervalo_s"] | 0;
    String s;
    serializeJson(det, s);
    n->details = s;
  }
  publicar_doc(topico(mac, "status"), d, true);
}

void publicar_resultado(const uint8_t mac[6], JsonDocument& r) {
  const char* command_id = r["command_id"];
  bool aplicado = r["aplicado"] | false;
  quitar_en_curso(mac, command_id);
  publicar_respuesta(mac, "recibido", command_id, NULL, JsonVariantConst());
  publicar_respuesta(mac, aplicado ? "aplicado" : "rechazado", command_id,
                     r["motivo"] | "", r["config"]);
}

void publicar_alerta(const uint8_t mac[6], JsonDocument& r) {
  const char* tipo = r["tipo"] | "";
  if (!raiz_alerta_tipo_valido(tipo)) {
    Serial.printf("[ALERTA] !! tipo invalido '%s', no se publica\n", tipo);
    return;
  }
  JsonDocument d;
  d["severity"] = r["severity"] | "unknown";
  d["message"]  = r["message"] | "";
  d["details"]  = r["details"];
  char iso[GW_ISO_BYTES];
  if (gw_epoca_a_iso(ahora_epoca(), iso)) d["ts"] = iso;
  publicar_doc(topico(mac, (String("alerts/") + tipo).c_str()), d, false);
}

void marcar_visto(const uint8_t mac[6], EstadoNodo* n) {
  if (!n) return;
  n->ultimo_ms = millis();
  if (n->offline) {   // volvio: online con los ultimos details conocidos
    n->offline = false;
    String cuerpo = String("{\"status\":\"online\",\"details\":") +
                    (n->details.length() ? n->details : String("{}")) + "}";
    publicar(topico(mac, "status"), cuerpo, true);
  }
}

void procesar(const RecibidaAura* r) {
  if (!aura_tipo_sube(r->t.tipo)) return;
  EstadoNodo* n = nodo(r->de, true);
  marcar_visto(r->de, n);

  if (r->t.tipo == AURA_TIPO_TELEMETRIA) {   // cada reintento se publica: AURA deduplica
    subir_telemetria(r->de, &r->t);
    return;
  }
  if (n && n->tiene_seq && n->ultima_seq == r->t.seq) { repetidas++; return; }
  if (n) { n->tiene_seq = true; n->ultima_seq = r->t.seq; }

  char json[AURA_PAYLOAD_MAX + 1];
  aura_payload_texto(&r->t, json, sizeof(json));
  JsonDocument d;
  if (deserializeJson(d, json)) { values_invalidos++; return; }
  if (r->t.tipo == AURA_TIPO_REPORTE)        publicar_reporte(r->de, n, d);
  else if (r->t.tipo == AURA_TIPO_RESULTADO) publicar_resultado(r->de, d);
  else if (r->t.tipo == AURA_TIPO_ALERTA)    publicar_alerta(r->de, d);
}

// ===== BAJADA: lo que manda AURA =====
void procesar_ack(const uint8_t mac[6], const String& msg) {
  JsonDocument d;
  uint8_t id[AURA_INGEST_ID_BYTES];
  const char* resultado = NULL;
  if (!deserializeJson(d, msg)) resultado = d["resultado"];
  // Ante cualquier duda no se confirma: la hoja reintenta, y AURA contesta
  // "duplicado". Confirmar de mas borraria una muestra.
  if (!raiz_ack_confirma(resultado) || !raiz_ingest_id_de_texto(d["ingest_id"] | "", id)) {
    acks_ignorados++;
    Serial.printf("[ACK] !! ignorado: %s\n", msg.c_str());
    return;
  }
  TramaAura t;
  aura_confirmacion_armar(&t, seq_actual++, id, ahora_epoca());
  if (radio_a(mac, &t)) confirmadas++;
  else Serial.println("[ACK] la hoja no esta alcanzable; reintentara y AURA dira duplicado");
}

void procesar_comando(const uint8_t mac[6], const String& msg) {
  JsonDocument d;
  if (deserializeJson(d, msg) || !d["command"].is<const char*>()) {
    publicar_respuesta(mac, "rechazado", NULL, "json_invalido", JsonVariantConst());
    return;
  }
  const char* command_id = d["command_id"] | "";
  if (memcmp(mac, radio_mi_mac(), 6) == 0) {
    publicar_respuesta(mac, "rechazado", command_id, "el_raiz_no_acepta_comandos", JsonVariantConst());
    return;
  }
  if (msg.length() > AURA_PAYLOAD_MAX) {   // no se trunca: llegaria otro comando
    publicar_respuesta(mac, "rechazado", command_id, "comando_muy_grande", JsonVariantConst());
    return;
  }
  TramaAura t;
  aura_trama_init(&t, AURA_TIPO_COMANDO, seq_actual++, (const uint8_t*)msg.c_str(), (uint8_t)msg.length());
  if (!radio_a(mac, &t)) {
    publicar_respuesta(mac, "rechazado", command_id, "nodo_no_alcanzable", JsonVariantConst());
    return;
  }
  publicar_respuesta(mac, "transmitido", command_id, NULL, JsonVariantConst());
  for (int i = 0; i < EN_CURSO_CAP; i++) {
    if (en_curso[i].activo) continue;
    en_curso[i].activo = true;
    memcpy(en_curso[i].mac, mac, 6);
    en_curso[i].command_id = command_id;
    en_curso[i].desde = millis();
    break;
  }
}

// Corre dentro de mqtt.poll(), en el loop: puede publicar y usar la radio.
void on_mqtt(int size) {
  String top = mqtt.messageTopic();
  String msg;
  msg.reserve(size);
  while (mqtt.available()) msg += (char)mqtt.read();

  uint8_t mac[6];
  char tipo[RAIZ_TIPO_MAX];
  if (!raiz_parsear_topico(top.c_str(), mac, tipo)) { ajenos++; return; }   // LoRaWAN u otro
  Serial.printf("[MQTT] RX %s %s\n", top.c_str(), msg.c_str());
  if (strcmp(tipo, "ack") == 0) procesar_ack(mac, msg);
  else procesar_comando(mac, msg);
}

// ===== VIGILANCIA =====
void vencer_comandos() {
  for (int i = 0; i < EN_CURSO_CAP; i++) {
    if (!en_curso[i].activo || millis() - en_curso[i].desde < ESPERA_RESULTADO) continue;
    en_curso[i].activo = false;
    publicar_respuesta(en_curso[i].mac, "rechazado", en_curso[i].command_id.c_str(),
                       "sin_resultado_del_nodo", JsonVariantConst());
  }
}

void revisar_offline() {
  for (int i = 0; i < NODOS_CAP; i++) {
    EstadoNodo* n = &nodos[i];
    if (!n->usado || !gw_debe_marcar_offline(millis(), n->ultimo_ms, n->intervalo_s, n->offline)) continue;
    n->offline = true;
    publicar(topico(n->mac, "status"), "{\"status\":\"offline\",\"details\":{\"motivo\":\"sin_tramas\"}}", true);
  }
}

void publicar_status_raiz() {
  JsonDocument d;
  d["status"] = "online";
  JsonObject det = d["details"].to<JsonObject>();
  char m[18];
  uint8_t canal;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&canal, &sec);
  det["transporte"]       = "mesh";
  det["rol"]              = "raiz";
  det["mac"]              = aura_mac_texto(radio_mi_mac(), m);
  det["canal"]            = canal;
  det["nodos_en_mesh"]    = radio_nodos_en_mesh();
  det["publicadas"]       = publicadas;
  det["confirmadas"]      = confirmadas;
  det["acks_ignorados"]   = acks_ignorados;
  det["values_invalidos"] = values_invalidos;
  det["repetidas"]        = repetidas;
  det["ajenos"]           = ajenos;
  det["sin_lugar"]        = sin_lugar;
  det["version_distinta"] = radio_aura.version_distinta;
  det["invalidas"]        = radio_aura.invalidas;
  det["desbordes_radio"]  = radio_aura.desbordes;
  det["hora_valida"]      = ahora_epoca() != 0;
  det["uptime_s"]         = millis() / 1000;
  publicar_doc(topico(radio_mi_mac(), "status"), d, true);
}

// ===== CONEXION =====
bool conectar_mqtt() {
  // Last Will: si esta placa muere, el broker publica offline por ella.
  String top = topico(radio_mi_mac(), "status");
  const char* lwt = "{\"status\":\"offline\"}";
  mqtt.setId(mi_hw);
  mqtt.beginWill(top, strlen(lwt), true, 1);
  mqtt.print(lwt);
  mqtt.endWill();

  if (!mqtt.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.printf("[MQTT] !! fallo la conexion, error %d. Con WiFi conectado, lo mas probable\n"
                  "       es aislamiento de clientes en la red de invitados.\n", mqtt.connectError());
    return false;
  }
  mqtt.subscribe("hw/+/ack", 1);
  mqtt.subscribe("hw/+/command", 1);
  Serial.println("[MQTT] conectado y suscripto a hw/+/ack y hw/+/command");
  publicar_status_raiz();
  return true;
}

void mantener_conexiones() {
  static bool hora_pedida = false;
  static unsigned long ultimo_intento = 0;
  if (!radio_conectada()) return;   // la mesh reconecta al router sola
  if (radio_reconecto()) {
    Serial.printf("[RAIZ] con IP %s, RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (!hora_pedida) {             // la hora: para "ts" y para darsela a las hojas
      configTime(0, 0, NTP_SERVIDOR);
      hora_pedida = true;
    }
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

  AuraMeshConfig mc = {{0}, MESH_CLAVE, MESH_CANAL, ROUTER_SSID, ROUTER_CLAVE};
  memcpy(mc.mesh_id, mesh_id, 6);
  if (aura_mac_vacia(mesh_id)) Serial.println("AVISO: MESH_ID sin configurar, falta config_local.h");
  if (!radio_iniciar(AURA_ROL_RAIZ, &mc, 32)) {
    Serial.println("no arranco la mesh, reinicio");
    delay(2000);
    ESP.restart();
  }
  radio_verificar_placa(mac_esperada);
  raiz_hw_id_de_mac(radio_mi_mac(), mi_hw);
  mqtt.onMessage(on_mqtt);
  Serial.printf("nodo_raiz listo: hw_id %s, esperando al router %s\n", mi_hw, ROUTER_SSID);
}

// ===== LOOP =====
void loop() {
  RecibidaAura r;
  while (radio_recibir(&r)) procesar(&r);
  mantener_conexiones();
  if (mqtt.connected()) mqtt.poll();

  static unsigned long ultimo_chequeo = 0;
  if (millis() - ultimo_chequeo > 5000) {
    ultimo_chequeo = millis();
    revisar_offline();
    vencer_comandos();
  }

  static unsigned long ultimo_status = 0;
  if (mqtt.connected() && millis() - ultimo_status > INTERVALO_STATUS) {
    ultimo_status = millis();
    publicar_status_raiz();
  }
  delay(5);
}
