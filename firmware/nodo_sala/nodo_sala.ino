/*
 * AURA - nodo de sala
 *
 * Gobierna la habitacion y es el unico nodo con dos vecinos: recibe del
 * sensor y reenvia al gateway, y baja los comandos en sentido inverso.
 *
 * La regla que evita logica por caso: si mac_destino es la propia, procesar;
 * si no, reenviar al otro vecino sin interpretar el contenido.
 *
 * SIN BIBLIOTECAS EXTERNAS.
 * IDE: Placa "XIAO_ESP32S3". USB CDC On Boot: ENABLED, si no el monitor
 * serie no muestra nada.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "../comun/protocolo_aura.h"
#include "../comun/buffer_circular.h"

// ===== CONFIGURACION =====
// Cambiar por las MAC reales antes de flashear.
uint8_t MAC_SENSOR[6]  = {0xE0, 0x72, 0xA1, 0xF7, 0xEF, 0xE4};
uint8_t MAC_GATEWAY[6] = {0xE0, 0x72, 0xA1, 0xD8, 0x48, 0xB0};

// MAC de la placa en la que DEBE correr este sketch.
const uint8_t MAC_ESPERADA[6] = {0xE0, 0x72, 0xA1, 0xF7, 0xF5, 0x48};

// MODO_BANCO en 1: probar sensor + sala SIN el gateway encendido. Desactiva
// la subida y, sobre todo, el barrido de canales: sin esto la sala no
// recibiria ACK del gateway y al minuto se pondria a saltar de canal,
// rompiendo el enlace con el sensor que si funciona.
// PONER EN 0 cuando el gateway este en linea.
#define MODO_BANCO 0

// 4 s, no 1,5: el gateway publica por MQTT en el mismo loop en que
// despacha los ACK, y una publicacion TCP lenta puede demorarlos. Un
// timeout corto hacia retransmitir de gusto y ensuciaba el broker.
const unsigned long TIMEOUT_ACK = 4000;
const unsigned long SIN_ACK_MAX = 60000;

BufferCircular hacia_arriba;

uint8_t  mi_mac[6];
uint16_t seq_actual = 0;

volatile bool     ack_recibido = false;
volatile uint16_t ack_seq      = 0;

float ultimo_lux = -1;
unsigned long ultimo_ack_ok = 0;

// Los comandos NO se reenvian dentro del callback: ese corre en la tarea de
// WiFi y esperar el ACK ahi bloquearia la radio. Se dejan en esta ranura y
// el loop los reenvia.
TramaAura comando_pendiente;
volatile bool hay_comando_pendiente = false;

const char* mac_str(const uint8_t mac[6]) {
  static char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return b;
}

void aplicar_comando(const char* json);

// Una MAC en cero significa "todavia no configurada". Sin este chequeo, el
// nodo intentaria hablarle a 00:00:00:00:00:00, nunca recibiria ACK y a los
// 60 s se pondria a barrer canales, rompiendo el enlace que SI funciona.
bool mac_configurada(const uint8_t mac[6]) {
  for (int i = 0; i < 6; i++) if (mac[i] != 0x00) return true;
  return false;
}

// ===== ACK =====
void mandar_ack(const uint8_t destino[6], uint16_t seq) {
  TramaAura ack;
  aura_trama_init(&ack, AURA_TIPO_ACK, mi_mac, destino, seq, NULL, 0);
  esp_now_send(destino, (const uint8_t*)&ack, aura_trama_bytes(&ack));
}

// ===== RECEPCION =====
void on_recv(const esp_now_recv_info_t* info, const uint8_t* datos, int len) {
  if (!aura_trama_valida(datos, len)) return;

  // Quien nos transmitio, que no es lo mismo que el origen del dato: las
  // confirmaciones son salto a salto y van siempre a este.
  const uint8_t* salto_anterior = info->src_addr;

  const TramaAura* t = (const TramaAura*)datos;

  if (t->tipo == AURA_TIPO_ACK) {
    if (aura_es_para_mi(t, mi_mac)) {
      ack_seq = t->seq;
      ack_recibido = true;
    }
    return;
  }

  if (t->tipo == AURA_TIPO_TELEMETRIA) {
    // Se confirma al sensor apenas se acepta la trama, aunque todavia no
    // se haya subido: el buffer de esta placa se hace cargo desde aca.
    // El payload viene del aire y no esta terminado en cero: copiar antes.
    char json[AURA_PAYLOAD_MAX + 1];
    memcpy(json, t->payload, t->largo);
    json[t->largo] = '\0';
    sscanf(json, "{\"lux\":%f", &ultimo_lux);

    Serial.printf("[SUBE] RX  telemetria de %s  seq=%u  %s\n",
                  mac_str(t->mac_origen), t->seq, json);

    // Se confirma al sensor apenas se acepta la trama, aunque todavia no
    // se haya subido: el buffer de esta placa se hace cargo desde aca.
    mandar_ack(salto_anterior, t->seq);
    Serial.printf("[SUBE] TX  ack -> %s  seq=%u\n", mac_str(salto_anterior), t->seq);

    buffer_push(&hacia_arriba, t);
    return;
  }

  if (t->tipo == AURA_TIPO_COMANDO) {
    Serial.printf("[BAJA] RX  comando de %s  seq=%u  destino=%s\n",
                  mac_str(t->mac_origen), t->seq, mac_str(t->mac_destino));
    mandar_ack(salto_anterior, t->seq);
    Serial.printf("[BAJA] TX  ack -> %s  seq=%u\n", mac_str(salto_anterior), t->seq);

    if (aura_es_para_mi(t, mi_mac)) {
      char json[AURA_PAYLOAD_MAX + 1];
      memcpy(json, t->payload, t->largo);
      json[t->largo] = '\0';
      Serial.println("[BAJA] --  el destino soy yo");
      aplicar_comando(json);
    } else {
      Serial.println("[BAJA] --  el destino es el sensor, lo encolo para reenviar");
      comando_pendiente = *t;
      hay_comando_pendiente = true;
    }
  }
}

void aplicar_comando(const char* json) {
  Serial.printf("[BAJA] OK  aplicado en esta sala: %s\n", json);
  // Pendiente de producto: mapear a los actuadores reales de la habitacion.
}

// ===== ENVIO =====
bool enviar_con_ack(const uint8_t destino[6], const TramaAura* t) {
  ack_recibido = false;
  if (esp_now_send(destino, (const uint8_t*)t, aura_trama_bytes(t)) != ESP_OK) return false;

  unsigned long inicio = millis();
  while (millis() - inicio < TIMEOUT_ACK) {
    if (ack_recibido && ack_seq == t->seq) {
      ultimo_ack_ok = millis();
      return true;
    }
    delay(10);
  }
  return false;
}

void buscar_gateway_por_canales() {
  // Se recuerda el canal actual: si el barrido fracasa hay que volver aca.
  // Sin esto la radio quedaba abandonada en el canal 13, hablandole a nadie.
  uint8_t canal_original;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&canal_original, &sec);

  Serial.printf("enlace con el gateway perdido, barriendo canales... (estoy en el %u)\n", canal_original);

  for (uint8_t canal = 1; canal <= 13; canal++) {
    esp_wifi_set_channel(canal, WIFI_SECOND_CHAN_NONE);
    delay(120);

    TramaAura ping;
    aura_trama_init(&ping, AURA_TIPO_TELEMETRIA, mi_mac, MAC_GATEWAY, seq_actual, NULL, 0);
    ack_recibido = false;
    esp_now_send(MAC_GATEWAY, (const uint8_t*)&ping, aura_trama_bytes(&ping));

    unsigned long inicio = millis();
    while (millis() - inicio < 300) {
      if (ack_recibido) {
        Serial.printf("gateway encontrado en canal %d\n", canal);
        ultimo_ack_ok = millis();
        return;
      }
      delay(10);
    }
  }
  esp_wifi_set_channel(canal_original, WIFI_SECOND_CHAN_NONE);
  Serial.printf("no se encontro al gateway en ningun canal, vuelvo al canal %u\n", canal_original);
}

// Verificacion de placa: con varias placas identicas es facilisimo flashear
// el sketch equivocado, y el sintoma (no llega nada) parece un problema de
// radio. Esto lo detecta en el arranque y lo dice con todas las letras.
void verificar_placa() {
  if (memcmp(mi_mac, MAC_ESPERADA, 6) == 0) return;

  Serial.println();
  Serial.println("****************************************************");
  Serial.println("*** PLACA EQUIVOCADA                             ***");
  Serial.printf ("*** esta placa es  %02X:%02X:%02X:%02X:%02X:%02X            ***\n",
                 mi_mac[0], mi_mac[1], mi_mac[2], mi_mac[3], mi_mac[4], mi_mac[5]);
  Serial.printf ("*** este sketch es para %02X:%02X:%02X:%02X:%02X:%02X       ***\n",
                 MAC_ESPERADA[0], MAC_ESPERADA[1], MAC_ESPERADA[2],
                 MAC_ESPERADA[3], MAC_ESPERADA[4], MAC_ESPERADA[5]);
  Serial.println("*** No va a llegar nada. Revisa la etiqueta.     ***");
  Serial.println("****************************************************");
  Serial.println();
}

void agregar_peer(const uint8_t mac[6]) {
  esp_now_peer_info_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_get_mac(WIFI_IF_STA, mi_mac);

  buffer_init(&hacia_arriba);
  ultimo_ack_ok = millis();

  if (esp_now_init() != ESP_OK) {
    Serial.println("fallo esp_now_init");
    delay(2000);
    ESP.restart();
  }
  esp_now_register_recv_cb(on_recv);
  agregar_peer(MAC_SENSOR);
  agregar_peer(MAC_GATEWAY);

  verificar_placa();

  if (MODO_BANCO)
    Serial.println("*** MODO_BANCO=1: no se sube al gateway ni se barren canales. ***");
  else if (!mac_configurada(MAC_GATEWAY))
    Serial.println("AVISO: MAC_GATEWAY sin configurar -> no se sube nada.");

  Serial.printf("nodo_sala listo, mi MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                mi_mac[0], mi_mac[1], mi_mac[2], mi_mac[3], mi_mac[4], mi_mac[5]);
}

// ===== LOOP =====
void loop() {
  TramaAura t;

  // Primero lo que baja: un comando esperando es mas urgente que la telemetria.
  if (hay_comando_pendiente) {
    hay_comando_pendiente = false;
    Serial.printf("[BAJA] TX  comando -> sensor  seq=%u\n", comando_pendiente.seq);
    if (enviar_con_ack(MAC_SENSOR, &comando_pendiente))
      Serial.printf("[BAJA] OK  ack del sensor  seq=%u\n", comando_pendiente.seq);
    else
      Serial.printf("[BAJA] !!  el sensor NO confirmo  seq=%u\n", comando_pendiente.seq);
  }

  if (!MODO_BANCO && mac_configurada(MAC_GATEWAY)) {
    if (buffer_peek(&hacia_arriba, &t)) {
      Serial.printf("[SUBE] TX  telemetria -> gateway  seq=%u  (en cola %u)\n",
                    t.seq, buffer_cantidad(&hacia_arriba));
      if (enviar_con_ack(MAC_GATEWAY, &t)) {
        buffer_pop(&hacia_arriba, &t);
        Serial.printf("[SUBE] OK  ack del gateway  seq=%u\n", t.seq);
      } else {
        Serial.printf("[SUBE] !!  el gateway NO confirmo  seq=%u, se reintenta\n", t.seq);
        delay(500);
      }
    }
    // Solo se concluye que el enlace murio si hay datos SIN CONFIRMAR.
  // Estando ocioso, ultimo_ack_ok no se refresca nunca y el barrido se
  // disparaba solo, rompiendo un enlace que estaba perfecto.
  if (buffer_cantidad(&hacia_arriba) > 0 && millis() - ultimo_ack_ok > SIN_ACK_MAX)
    buscar_gateway_por_canales();
  } else {
    // Modo banco: sin gateway, se muestra el dato y se descarta. No se barren
    // canales, para no romper el enlace con el sensor.
    buffer_pop(&hacia_arriba, &t);
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
