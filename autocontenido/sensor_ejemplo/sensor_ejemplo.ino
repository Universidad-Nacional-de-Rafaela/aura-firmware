// GENERADO por herramientas/generar_autocontenidos.sh: no editar. Fuente: dispositivos/sensor_ejemplo/
/*
 * AURA - nodo sensor (XIAO ESP32S3)
 *
 * Manda su muestra al padre (el nodo de sala) por ESP-NOW. No conoce AURA:
 * se identifica por MAC y no sabe que es un UUID ni MQTT.
 *
 * SIN BIBLIOTECAS EXTERNAS. Con la placa pelada, lo unico fisico que se puede
 * medir es la temperatura interna del chip (temperatureRead(), incluida en el
 * core). Alcanza para validar la cadena de punta a punta: apretando el chip
 * con el dedo se ve subir el valor hasta la base de datos.
 *
 * El lux va simulado con una rampa, para no romper el contrato con AURA
 * (el gateway publica este nodo como type "lux"). Cuando haya un BH1750,
 * se reemplaza lux_simulado() y listo.
 *
 * IDE: Placa "XIAO_ESP32S3". USB CDC On Boot: ENABLED, si no el monitor
 * serie no muestra nada.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "protocolo_aura.h"
#include "buffer_circular.h"

// ===== CONFIGURACION =====
// MAC del nodo de sala.
uint8_t MAC_PADRE[6] = {0xE0, 0x72, 0xA1, 0xF7, 0xF5, 0x48};

// MAC de la placa en la que DEBE correr este sketch.
const uint8_t MAC_ESPERADA[6] = {0xE0, 0x72, 0xA1, 0xF7, 0xEF, 0xE4};

// Cada cuanto emite. 3000 para ver movimiento rapido en el banco,
// 60000 para dejarlo corriendo sin inundar la base.
const unsigned long INTERVALO_MUESTRA = 10000;  // 10 s
// 4 s, no 1,5: el gateway publica por MQTT en el mismo loop en que despacha
// los ACK, y una publicacion TCP lenta puede demorarlos. Un timeout corto
// hacia retransmitir de gusto.
const unsigned long TIMEOUT_ACK       = 4000;
const unsigned long SIN_ACK_MAX       = 60000;  // 1 min sin ACK -> barrer canales

BufferCircular pendientes;

uint8_t  mi_mac[6];
uint16_t seq_actual = 0;

volatile bool     ack_recibido = false;
volatile uint16_t ack_seq      = 0;

unsigned long ultima_muestra = 0;
unsigned long ultimo_ack_ok  = 0;

// Rampa lenta de 0 a 500 y vuelta, para que en el monitor de la sala se vea
// que el valor cambia y no parezca congelado.
float lux_simulado() {
  unsigned long fase = (millis() / 1000) % 120;
  float x = (fase < 60) ? fase : (120 - fase);
  return x * (500.0f / 60.0f);
}

// ===== RECEPCION =====
void mandar_ack(const uint8_t destino[6], uint16_t seq) {
  TramaAura ack;
  aura_trama_init(&ack, AURA_TIPO_ACK, mi_mac, destino, seq, NULL, 0);
  esp_now_send(destino, (const uint8_t*)&ack, aura_trama_bytes(&ack));
}

void on_recv(const esp_now_recv_info_t* info, const uint8_t* datos, int len) {
  if (!aura_trama_valida(datos, len)) return;

  // Salto anterior: a quien hay que confirmarle. Aca coincide siempre con
  // MAC_PADRE, pero se usa el mismo criterio que en los otros nodos.
  const uint8_t* salto_anterior = info->src_addr;

  const TramaAura* t = (const TramaAura*)datos;

  if (t->tipo == AURA_TIPO_ACK) {
    if (aura_es_para_mi(t, mi_mac)) {
      ack_seq = t->seq;
      ack_recibido = true;
    }
    return;
  }

  // Comandos que bajan de AURA. El ACK va al padre (el salto anterior), no
  // al origen: las confirmaciones son salto a salto.
  if (t->tipo == AURA_TIPO_COMANDO && aura_es_para_mi(t, mi_mac)) {
    mandar_ack(salto_anterior, t->seq);

    char json[AURA_PAYLOAD_MAX + 1];
    memcpy(json, t->payload, t->largo);
    json[t->largo] = '\0';
    Serial.printf("comando recibido: %s\n", json);
  }
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

// ===== ENVIO =====
bool enviar_con_ack(const TramaAura* t) {
  ack_recibido = false;
  if (esp_now_send(MAC_PADRE, (const uint8_t*)t, aura_trama_bytes(t)) != ESP_OK) return false;

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

// ===== RECUPERACION DE CANAL =====
// ESP-NOW solo transmite en el canal activo. Si el AP cambia de canal, el
// padre deja de escucharnos sin que aparezca ningun error: hay que buscarlo.
void buscar_padre_por_canales() {
  // Se recuerda el canal actual: si el barrido fracasa hay que volver aca.
  // Sin esto la radio quedaba abandonada en el canal 13, hablandole a nadie.
  uint8_t canal_original;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&canal_original, &sec);

  Serial.printf("enlace perdido, barriendo canales... (estoy en el %u)\n", canal_original);

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
  esp_wifi_set_channel(canal_original, WIFI_SECOND_CHAN_NONE);
  Serial.printf("no se encontro al padre en ningun canal, vuelvo al canal %u\n", canal_original);
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  // Sin esto el receptor se apaga por intervalos (modem-sleep) y se pierden
  // los ACK aunque el envio funcione: la falla se ve como asimetrica.
  WiFi.setSleep(false);
  esp_wifi_get_mac(WIFI_IF_STA, mi_mac);

  buffer_init(&pendientes);
  ultimo_ack_ok = millis();

  if (esp_now_init() != ESP_OK) {
    Serial.println("fallo esp_now_init");
    delay(2000);
    ESP.restart();
  }
  esp_now_register_recv_cb(on_recv);

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, MAC_PADRE, 6);
  peer.channel = 0;  // 0 = canal actual
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  verificar_placa();

  Serial.printf("nodo_sensor listo, mi MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                mi_mac[0], mi_mac[1], mi_mac[2], mi_mac[3], mi_mac[4], mi_mac[5]);
  Serial.printf("padre configurado:      %02X:%02X:%02X:%02X:%02X:%02X\n",
                MAC_PADRE[0], MAC_PADRE[1], MAC_PADRE[2],
                MAC_PADRE[3], MAC_PADRE[4], MAC_PADRE[5]);
  Serial.printf("temperatura interna inicial: %.1f C\n", temperatureRead());
}

// ===== LOOP =====
void loop() {
  unsigned long ahora = millis();

  if (ahora - ultima_muestra >= INTERVALO_MUESTRA) {
    ultima_muestra = ahora;

    float temp = temperatureRead();   // medicion real, sin componentes
    float lux  = lux_simulado();      // rampa, hasta que haya un BH1750

    // pend y desc viajan DENTRO del dato: corriendo con fuente y sin monitor
    // serie, esta es la unica forma de saber si los ACK estan volviendo.
    char json[110];
    int n = snprintf(json, sizeof(json),
                     "{\"lux\":%.1f,\"temp_c\":%.1f,\"pend\":%u,\"desc\":%lu,\"lux_sim\":true}",
                     lux, temp, buffer_cantidad(&pendientes),
                     (unsigned long)buffer_descartados(&pendientes));

    TramaAura t;
    aura_trama_init(&t, AURA_TIPO_TELEMETRIA, mi_mac, MAC_PADRE,
                    seq_actual++, (const uint8_t*)json, (uint8_t)n);
    buffer_push(&pendientes, &t);

    Serial.printf("muestra %.1f lux  %.1f C  pendientes=%u  descartados=%lu\n",
                  lux, temp, buffer_cantidad(&pendientes),
                  (unsigned long)buffer_descartados(&pendientes));
  }

  // Solo se saca del buffer lo que el padre confirmo.
  TramaAura t;
  if (buffer_peek(&pendientes, &t)) {
    if (enviar_con_ack(&t)) {
      buffer_pop(&pendientes, &t);
      Serial.printf("  ack OK del padre  seq=%u  -> quedan %u sin confirmar\n",
                    t.seq, buffer_cantidad(&pendientes));
    } else {
      Serial.printf("  SIN ack  seq=%u  -> quedan %u sin confirmar\n",
                    t.seq, buffer_cantidad(&pendientes));
      delay(500);
    }
  }

  // Solo se concluye que el enlace murio si hay datos SIN CONFIRMAR.
  // Estando ocioso, ultimo_ack_ok no se refresca nunca y el barrido se
  // disparaba solo, rompiendo un enlace que estaba perfecto.
  if (buffer_cantidad(&pendientes) > 0 && millis() - ultimo_ack_ok > SIN_ACK_MAX)
    buscar_padre_por_canales();

  delay(50);
}
