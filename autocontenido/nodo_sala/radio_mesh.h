#pragma once
// Radio ESP-NOW compartida por los tres roles de la mesh (nodo, sala, gateway).
// Solo compila en la placa. Junta las reglas que salieron del banco de la v1
// (docs/mesh-v1, §9.2):
//   1. El callback de recepcion solo valida y encola: nunca envia ni bloquea.
//   2. El barrido de canales solo actua ante evidencia de falla (lo decide quien llama).
//   3. El barrido restaura el canal en todos sus caminos de salida.
//   4. Los ACK van al salto anterior (info->src_addr), nunca al origen del dato.
//   5. Cada sketch declara en que placa debe correr y lo grita si no coincide.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "protocolo_aura.h"

// Una trama recibida, con quien la transmitio (el salto anterior) y su RSSI.
typedef struct {
  TramaAura t;
  uint8_t   de[6];
  int8_t    rssi;
} RecibidaAura;

typedef struct {
  uint8_t           mi_mac[6];
  QueueHandle_t     cola;
  volatile bool     ack_recibido;
  volatile uint16_t ack_seq;
  // Lo que se descarta en el callback, contado para que no sea silencioso.
  volatile uint32_t version_distinta;  // trama de otra generacion de firmware
  volatile uint32_t invalidas;         // trama rota o que no es de AURA
  volatile uint32_t desbordes;         // la cola de recepcion estaba llena
} RadioAura;

static RadioAura radio_aura;

static inline bool aura_mac_vacia(const uint8_t mac[6]) {
  for (int i = 0; i < 6; i++) if (mac[i] != 0x00) return false;
  return true;
}

// Con buffer propio: con uno estatico, dos MAC en el mismo printf salian iguales.
static inline const char* aura_mac_texto(const uint8_t mac[6], char salida[18]) {
  snprintf(salida, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return salida;
}

// Corre en la tarea de WiFi: solo valida y encola. Los ACK de salto se
// anotan aca porque quien espera uno esta haciendo polling en el loop.
static void radio_on_recv(const esp_now_recv_info_t* info, const uint8_t* datos, int len) {
  AuraValidacion v = aura_trama_validar(datos, len);
  // "x = x + 1" y no "x++": ++ sobre volatile esta deprecado en C++20.
  if (v == AURA_TRAMA_VERSION) {
    radio_aura.version_distinta = radio_aura.version_distinta + 1;
    return;
  }
  if (v != AURA_TRAMA_OK) {
    radio_aura.invalidas = radio_aura.invalidas + 1;
    return;
  }

  const TramaAura* t = (const TramaAura*)datos;
  if (t->tipo == AURA_TIPO_ACK) {
    if (aura_es_para_mi(t, radio_aura.mi_mac)) {
      radio_aura.ack_seq = t->seq;
      radio_aura.ack_recibido = true;
    }
    return;
  }

  RecibidaAura r;
  memset(&r, 0, sizeof(r));
  memcpy(&r.t, datos, len);
  memcpy(r.de, info->src_addr, 6);
  r.rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
  if (xQueueSend(radio_aura.cola, &r, 0) != pdTRUE) radio_aura.desbordes = radio_aura.desbordes + 1;
}

// Llamar con WiFi ya en modo STA. Devuelve false si ESP-NOW no arranco.
static inline bool radio_iniciar(uint8_t capacidad_cola) {
  memset(&radio_aura, 0, sizeof(radio_aura));
  esp_wifi_get_mac(WIFI_IF_STA, radio_aura.mi_mac);
  radio_aura.cola = xQueueCreate(capacidad_cola, sizeof(RecibidaAura));
  if (radio_aura.cola == NULL) return false;
  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(radio_on_recv);
  return true;
}

static inline const uint8_t* radio_mi_mac() { return radio_aura.mi_mac; }

static inline bool radio_recibir(RecibidaAura* r) {
  return xQueueReceive(radio_aura.cola, r, 0) == pdTRUE;
}

static inline void radio_agregar_peer(const uint8_t mac[6]) {
  if (aura_mac_vacia(mac) || esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;      // 0 = el canal en que este la radio
  p.encrypt = false;  // cifrado: pendiente 8 del contrato
  esp_now_add_peer(&p);
}

// Envia sin esperar confirmacion. true = la radio acepto la trama.
static inline bool radio_enviar(const uint8_t destino[6], const TramaAura* t) {
  radio_agregar_peer(destino);
  return esp_now_send(destino, (const uint8_t*)t, aura_trama_bytes(t)) == ESP_OK;
}

static inline void radio_mandar_ack(const uint8_t destino[6], uint16_t seq) {
  TramaAura ack;
  aura_trama_init(&ack, AURA_TIPO_ACK, radio_aura.mi_mac, destino, seq, NULL, 0);
  radio_enviar(destino, &ack);
}

// 4 s y no menos: el gateway puede estar en medio de un POST, y un timeout
// corto hace retransmitir de gusto (docs/mesh-v1).
#define AURA_TIMEOUT_ACK_MS 4000

// Envia al proximo salto y espera su ACK. Bloquea hasta timeout_ms.
static inline bool radio_enviar_con_ack(const uint8_t destino[6], const TramaAura* t,
                                        uint32_t timeout_ms = AURA_TIMEOUT_ACK_MS) {
  radio_aura.ack_recibido = false;
  if (!radio_enviar(destino, t)) return false;
  unsigned long inicio = millis();
  while (millis() - inicio < timeout_ms) {
    if (radio_aura.ack_recibido && radio_aura.ack_seq == t->seq) return true;
    delay(5);
  }
  return false;
}

// ESP-NOW solo transmite en el canal activo, que en quien tiene WiFi es el del
// AP. Si el AP cambia de canal, el padre deja de escuchar sin ningun error:
// hay que buscarlo. Restaura el canal original si no lo encuentra.
static inline bool radio_barrer_canales(const uint8_t destino[6], uint16_t seq) {
  uint8_t canal_original;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&canal_original, &sec);
  char m[18];
  Serial.printf("[MESH] enlace perdido con %s, barriendo canales (estoy en el %u)\n",
                aura_mac_texto(destino, m), canal_original);

  for (uint8_t canal = 1; canal <= 13; canal++) {
    esp_wifi_set_channel(canal, WIFI_SECOND_CHAN_NONE);
    delay(120);
    TramaAura ping;
    aura_trama_init(&ping, AURA_TIPO_PING, radio_aura.mi_mac, destino, seq, NULL, 0);
    if (radio_enviar_con_ack(destino, &ping, 300)) {
      Serial.printf("[MESH] encontrado en el canal %u\n", canal);
      return true;
    }
  }
  esp_wifi_set_channel(canal_original, WIFI_SECOND_CHAN_NONE);
  Serial.printf("[MESH] no aparecio en ningun canal, vuelvo al %u\n", canal_original);
  return false;
}

// Con placas identicas es facilisimo flashear el sketch equivocado, y el
// sintoma (no llega nada) parece un problema de radio. Una esperada en cero
// significa "sin configurar" y no se verifica.
static inline void radio_verificar_placa(const uint8_t esperada[6]) {
  if (aura_mac_vacia(esperada)) {
    Serial.println("[MESH] AVISO: MAC_ESPERADA sin configurar, no se verifica la placa");
    return;
  }
  if (memcmp(radio_aura.mi_mac, esperada, 6) == 0) return;
  char a[18], b[18];
  Serial.println();
  Serial.println("****************************************************");
  Serial.println("*** PLACA EQUIVOCADA                             ***");
  Serial.printf ("*** esta placa es       %s     ***\n", aura_mac_texto(radio_aura.mi_mac, a));
  Serial.printf ("*** este sketch es para %s     ***\n", aura_mac_texto(esperada, b));
  Serial.println("*** No va a llegar nada. Revisa la etiqueta.     ***");
  Serial.println("****************************************************");
  Serial.println();
}
