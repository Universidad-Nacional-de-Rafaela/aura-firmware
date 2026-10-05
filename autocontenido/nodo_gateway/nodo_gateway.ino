// GENERADO por herramientas/generar_autocontenidos.sh: no editar. Fuente: infraestructura/nodo_gateway/
/*
 * AURA - nodo gateway del piso (XIAO ESP32S3)
 *
 * Unico nodo que conoce AURA. Traduce MAC <-> UUID y publica al broker.
 *
 * DEPENDENCIAS: solo ArduinoMqttClient. No usa ArduinoJson: el JSON que
 * arma el sensor se reenvia como string y el poco JSON propio se construye
 * con snprintf.
 *
 * MODO_SIN_BACKEND=1 publica todo por MQTT y no toca la API REST: sirve para
 * ver la cadena completa con solo el broker levantado. En 0 usa
 * POST /api/v1/telemetry/ingest, que es idempotente y tolera reintentos.
 *
 * IDE: Placa "XIAO_ESP32S3". USB CDC On Boot: ENABLED.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <ArduinoMqttClient.h>
#include "protocolo_aura.h"
#include "buffer_circular.h"
#include "ingest_id.h"

// ===== CONFIGURACION =====
#define MODO_SIN_BACKEND 1

const char* WIFI_SSID = "CAMBIAR";   // SSID de la red WiFi
const char* WIFI_PASS = "CAMBIAR";   // clave de esa red

// IP de la maquina que corre el broker y la API. OJO: si esa maquina toma
// IP por DHCP puede cambiar y hay que reflashear.
const char* MQTT_HOST = "192.168.0.100";  // CAMBIAR
const int   MQTT_PORT = 1883;
const char* API_BASE  = "http://192.168.0.100:8000";  // CAMBIAR
const char* TENANT_ID = "550e8400-e29b-41d4-a716-446655440000";  // CAMBIAR

const char* GATEWAY_DEVICE_ID = "650e8400-e29b-41d4-a716-446655440003";  // CAMBIAR

const unsigned long INTERVALO_DIAG = 15000;
const int MAX_POR_LOTE = 10;

// MAC de la placa en la que DEBE correr este sketch.
const uint8_t MAC_ESPERADA[6] = {0xE0, 0x72, 0xA1, 0xD8, 0x48, 0xB0};

// Tabla MAC -> UUID: unica fuente de identidad de la mesh frente a AURA.
typedef struct {
  uint8_t     mac[6];
  const char* device_id;
  const char* tipo;
  bool        es_sala;   // por donde entran los comandos a la mesh
} NodoConocido;

NodoConocido TABLA[] = {
  // nodo sensor de luz  (etiqueta SENSOR)
  {{0xE0,0x72,0xA1,0xF7,0xEF,0xE4}, "650e8400-e29b-41d4-a716-446655440001", "lux",  false},
  // nodo de sala, puerta de entrada de los comandos (etiqueta SALA)
  {{0xE0,0x72,0xA1,0xF7,0xF5,0x48}, "650e8400-e29b-41d4-a716-446655440002", "sala", true },
};
const int TABLA_N = sizeof(TABLA) / sizeof(TABLA[0]);

WiFiClient net;
MqttClient mqtt(net);
BufferCircular por_subir;

uint8_t  mi_mac[6];
uint16_t seq_actual = 0;
uint32_t boot_id    = 0;

int      ultimo_codigo_http = 0;
uint32_t publicados         = 0;
uint32_t fallidos           = 0;
uint32_t duplicados         = 0;

// ACKs pendientes de enviar. NO se contesta desde el callback de recepcion:
// esa funcion corre en la tarea de WiFi y llamar ahi a esp_now_send() con el
// stack TCP ocupado publicando MQTT es donde se perdian los ACK. El callback
// solo encola; el loop envia.
typedef struct { uint8_t mac[6]; uint16_t seq; } AckPendiente;
#define ACKS_CAP 8
AckPendiente acks[ACKS_CAP];
volatile uint8_t acks_n = 0;

// Ultima seq publicada por nodo, para no republicar retransmisiones.
uint16_t ultima_seq[8];
bool     tiene_seq[8];

// ===== UTILIDADES =====
const char* mac_str(const uint8_t mac[6]) {
  static char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return b;
}

const NodoConocido* buscar(const uint8_t mac[6]) {
  for (int i = 0; i < TABLA_N; i++)
    if (memcmp(TABLA[i].mac, mac, 6) == 0) return &TABLA[i];
  return NULL;
}

const NodoConocido* nodo_de_entrada() {
  for (int i = 0; i < TABLA_N; i++)
    if (TABLA[i].es_sala) return &TABLA[i];
  return NULL;
}

// Copia el payload de una trama a un buffer terminado en cero.
void payload_a_texto(const TramaAura* t, char* salida, size_t cap) {
  size_t n = t->largo;
  if (n >= cap) n = cap - 1;
  memcpy(salida, t->payload, n);
  salida[n] = '\0';
}

void verificar_placa() {
  if (memcmp(mi_mac, MAC_ESPERADA, 6) == 0) return;
  Serial.println();
  Serial.println("****************************************************");
  Serial.println("*** PLACA EQUIVOCADA                             ***");
  Serial.printf ("*** esta placa es  %s            ***\n", mac_str(mi_mac));
  Serial.printf ("*** este sketch es para %s       ***\n", mac_str(MAC_ESPERADA));
  Serial.println("*** No va a llegar nada. Revisa la etiqueta.     ***");
  Serial.println("****************************************************");
  Serial.println();
}

// ===== ESP-NOW =====
void mandar_ack(const uint8_t destino[6], uint16_t seq) {
  TramaAura ack;
  aura_trama_init(&ack, AURA_TIPO_ACK, mi_mac, destino, seq, NULL, 0);
  esp_now_send(destino, (const uint8_t*)&ack, aura_trama_bytes(&ack));
}

void encolar_ack(const uint8_t mac[6], uint16_t seq) {
  if (acks_n >= ACKS_CAP) return;   // si se llena, la sala reintenta igual
  memcpy(acks[acks_n].mac, mac, 6);
  acks[acks_n].seq = seq;
  acks_n++;
}

void enviar_acks_pendientes() {
  while (acks_n > 0) {
    acks_n--;
    mandar_ack(acks[acks_n].mac, acks[acks_n].seq);
  }
}

void on_recv(const esp_now_recv_info_t* info, const uint8_t* datos, int len) {
  if (!aura_trama_valida(datos, len)) return;

  const TramaAura* t = (const TramaAura*)datos;
  if (t->tipo != AURA_TIPO_TELEMETRIA) return;

  // El ACK va al SALTO ANTERIOR, no al origen del dato. La sala reenvia la
  // trama del sensor sin modificarla, asi que mac_origen sigue siendo el
  // sensor: confirmarle a el dejaba a la sala esperando para siempre.
  // info->src_addr es quien transmitio realmente, que es a quien hay que
  // confirmarle.
  encolar_ack(info->src_addr, t->seq);
  if (t->largo > 0) buffer_push(&por_subir, t);  // largo 0 = ping de barrido
}

// ===== PUBLICACION POR MQTT =====
// El JSON del sensor se reenvia tal cual dentro de "values", que es lo que
// espera device_integration.py. Sin parsear nada: no hace falta ArduinoJson.
bool publicar_dato(const TramaAura* t) {
  const NodoConocido* nodo = buscar(t->mac_origen);
  if (!nodo) {
    Serial.printf("MAC desconocida %s, se descarta\n", mac_str(t->mac_origen));
    return true;  // se descarta igual, si no traba el buffer
  }

  // Una retransmision (la sala no recibio el ACK anterior) no se republica:
  // el dato ya salio y duplicarlo ensucia la serie temporal.
  int idx = (int)(nodo - TABLA);
  if (tiene_seq[idx] && ultima_seq[idx] == t->seq) {
    duplicados++;
    Serial.printf("[MQTT] -- seq=%u ya publicada, no se repite\n", t->seq);
    return true;
  }

  char json[AURA_PAYLOAD_MAX + 1];
  payload_a_texto(t, json, sizeof(json));

  char cuerpo[AURA_PAYLOAD_MAX + 64];
  snprintf(cuerpo, sizeof(cuerpo), "{\"values\":%s}", json);

  char topico[96];
  snprintf(topico, sizeof(topico), "devices/%s/data", nodo->device_id);

  mqtt.beginMessage(topico, false, 1);
  mqtt.print(cuerpo);
  int r = mqtt.endMessage();

  if (r == 1) {
    publicados++;
    ultima_seq[idx] = t->seq;
    tiene_seq[idx]  = true;
    Serial.printf("[MQTT] TX %s  seq=%u  %s\n", topico, t->seq, cuerpo);
    return true;
  }
  fallidos++;
  Serial.printf("[MQTT] !! fallo la publicacion seq=%u\n", t->seq);
  return false;
}

// ===== SUBIDA POR REST (MODO_SIN_BACKEND=0) =====
// Idempotente: el ingest_id se deriva de (mac_origen, boot_id, seq), asi que
// reintentar el mismo lote no agrega filas en ts_telemetry.
bool subir_lote_rest() {
  if (buffer_cantidad(&por_subir) == 0) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  static BufferCircular tmp;   // ~5,9 KB: en el stack de la loop task desborda
  tmp = por_subir;

  String cuerpo = "{\"events\":[";
  int n = 0, incluidos = 0;
  TramaAura t;

  while (n < MAX_POR_LOTE && buffer_pop(&tmp, &t)) {
    n++;
    const NodoConocido* nodo = buscar(t.mac_origen);
    if (!nodo) continue;

    char uuid[37];
    aura_ingest_id(t.mac_origen, boot_id, t.seq, uuid);

    char json[AURA_PAYLOAD_MAX + 1];
    payload_a_texto(&t, json, sizeof(json));

    char evento[AURA_PAYLOAD_MAX + 256];
    snprintf(evento, sizeof(evento),
             "%s{\"tenant_id\":\"%s\",\"device_id\":\"%s\",\"type\":\"%s\","
             "\"ingest_id\":\"%s\",\"payload\":%s}",
             incluidos ? "," : "", TENANT_ID, nodo->device_id, nodo->tipo, uuid, json);
    cuerpo += evento;
    incluidos++;
  }
  cuerpo += "]}";

  if (incluidos == 0) {
    for (int i = 0; i < n; i++) buffer_pop(&por_subir, &t);
    return true;
  }

  HTTPClient http;
  http.begin(String(API_BASE) + "/api/v1/telemetry/ingest");
  http.addHeader("Content-Type", "application/json");
  int codigo = http.POST(cuerpo);
  http.end();
  ultimo_codigo_http = codigo;

  if (codigo == 200 || codigo == 201) {
    for (int i = 0; i < n; i++) buffer_pop(&por_subir, &t);
    publicados += incluidos;
    Serial.printf("[REST] subidos %d eventos\n", incluidos);
    return true;
  }

  fallidos++;
  Serial.printf("[REST] !! fallo el POST (%d), se reintenta sin perder datos\n", codigo);
  return false;
}

// ===== COMANDOS QUE BAJAN =====
void on_mqtt(int size) {
  (void)size;
  String topico = mqtt.messageTopic();
  String msg;
  while (mqtt.available()) msg += (char)mqtt.read();

  for (int i = 0; i < TABLA_N; i++) {
    char esperado[96];
    snprintf(esperado, sizeof(esperado), "devices/%s/command", TABLA[i].device_id);
    if (topico != esperado) continue;

    Serial.printf("[BAJA] RX  %s  %s\n", esperado, msg.c_str());

    const NodoConocido* entrada = nodo_de_entrada();
    if (!entrada) { Serial.println("[BAJA] !! no hay nodo de entrada a la mesh"); return; }
    if (msg.length() > AURA_PAYLOAD_MAX) { Serial.println("[BAJA] !! comando demasiado largo"); return; }

    TramaAura t;
    aura_trama_init(&t, AURA_TIPO_COMANDO, mi_mac, TABLA[i].mac,
                    seq_actual++, (const uint8_t*)msg.c_str(), (uint8_t)msg.length());
    // Siempre entra por la sala; si el destino final es el sensor, ella reenvia.
    esp_now_send(entrada->mac, (const uint8_t*)&t, aura_trama_bytes(&t));
    Serial.printf("[BAJA] TX  a la mesh via %s  destino=%s  seq=%u\n",
                  mac_str(entrada->mac), mac_str(TABLA[i].mac), t.seq);

    char resp[96];
    snprintf(resp, sizeof(resp), "devices/%s/response", TABLA[i].device_id);
    mqtt.beginMessage(resp, false, 1);
    mqtt.print("{\"status\":\"enviado_a_mesh\"}");
    mqtt.endMessage();
    return;
  }
}

// ===== DIAGNOSTICO =====
// Todo lo que se veria por el monitor serie, pero por MQTT. Se sigue con:
//   mosquitto_sub -h $MQTT_HOST -t "devices/+/status" -v
void publicar_diagnostico() {
  uint8_t canal;
  wifi_second_chan_t segundo;
  esp_wifi_get_channel(&canal, &segundo);

  char cuerpo[256];
  // La MAC va en el status: sin esto, dos placas corriendo este sketch
  // publican al mismo topico y no hay forma de saber cual es cual.
  snprintf(cuerpo, sizeof(cuerpo),
           "{\"status\":\"online\",\"mac\":\"%s\",\"canal\":%u,\"rssi\":%d,\"pendientes\":%u,"
           "\"descartados\":%lu,\"publicados\":%lu,\"fallidos\":%lu,"
           "\"duplicados\":%lu,\"ultimo_http\":%d,\"uptime_s\":%lu}",
           mac_str(mi_mac), canal, (int)WiFi.RSSI(), buffer_cantidad(&por_subir),
           (unsigned long)buffer_descartados(&por_subir),
           (unsigned long)publicados, (unsigned long)fallidos,
           (unsigned long)duplicados,
           ultimo_codigo_http, (unsigned long)(millis() / 1000));

  char topico[96];
  snprintf(topico, sizeof(topico), "devices/%s/status", GATEWAY_DEVICE_ID);
  mqtt.beginMessage(topico, true, 1);
  mqtt.print(cuerpo);
  mqtt.endMessage();
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000);
  buffer_init(&por_subir);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("conectando a %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  WiFi.setSleep(false);

  esp_wifi_get_mac(WIFI_IF_STA, mi_mac);
  boot_id = esp_random();
  verificar_placa();

  uint8_t canal;
  wifi_second_chan_t segundo;
  esp_wifi_get_channel(&canal, &segundo);
  Serial.printf("\nWiFi ok  IP %s  RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  Serial.printf("CANAL DEL AP = %u  <- los nodos hoja tienen que estar aca\n", canal);

  if (esp_now_init() != ESP_OK) {
    Serial.println("fallo esp_now_init");
    delay(2000);
    ESP.restart();
  }
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

  // Last Will: si esta placa muere, el broker publica offline por ella.
  char top_estado[96];
  snprintf(top_estado, sizeof(top_estado), "devices/%s/status", GATEWAY_DEVICE_ID);
  const char* lwt = "{\"status\":\"offline\"}";
  mqtt.beginWill(top_estado, strlen(lwt), true, 1);
  mqtt.print(lwt);
  mqtt.endWill();

  Serial.printf("conectando al broker %s:%d ...\n", MQTT_HOST, MQTT_PORT);
  if (!mqtt.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.printf("!! FALLO la conexion MQTT, error %d\n", mqtt.connectError());
    Serial.println("!! Si el WiFi conecto pero esto falla, lo mas probable es");
    Serial.println("!! aislamiento de clientes en la red de invitados.");
  } else {
    Serial.println("broker conectado");
    for (int i = 0; i < TABLA_N; i++) {
      char top[96];
      snprintf(top, sizeof(top), "devices/%s/command", TABLA[i].device_id);
      mqtt.subscribe(top, 1);
      Serial.printf("suscrito a %s\n", top);
    }
    publicar_diagnostico();
  }

  Serial.println(MODO_SIN_BACKEND
    ? "nodo_gateway listo (MODO_SIN_BACKEND: publica por MQTT, no usa la API)"
    : "nodo_gateway listo (sube por REST a la API)");
}

// ===== LOOP =====
void loop() {
  enviar_acks_pendientes();   // primero confirmar: la mesh espera esto
  mqtt.poll();

  if (!mqtt.connected()) {
    Serial.println("MQTT caido, reconectando...");
    if (mqtt.connect(MQTT_HOST, MQTT_PORT)) {
      for (int i = 0; i < TABLA_N; i++) {
        char top[96];
        snprintf(top, sizeof(top), "devices/%s/command", TABLA[i].device_id);
        mqtt.subscribe(top, 1);
      }
      Serial.println("MQTT reconectado");
    } else {
      delay(2000);
      return;
    }
  }

  if (MODO_SIN_BACKEND) {
    TramaAura t;
    if (buffer_peek(&por_subir, &t)) {
      if (publicar_dato(&t)) buffer_pop(&por_subir, &t);
      else delay(500);
      // Otra vez despues de publicar: una trama que llego durante la
      // publicacion no tiene que esperar a la proxima vuelta del loop.
      enviar_acks_pendientes();
    }
  } else {
    static unsigned long ultimo_lote = 0;
    if (millis() - ultimo_lote > 5000) {
      ultimo_lote = millis();
      subir_lote_rest();
    }
  }

  static unsigned long ultimo_diag = 0;
  if (millis() - ultimo_diag > INTERVALO_DIAG) {
    ultimo_diag = millis();
    publicar_diagnostico();
    enviar_acks_pendientes();
  }

  delay(10);   // era 50: el loop es quien despacha los ACK, conviene rapido
}
