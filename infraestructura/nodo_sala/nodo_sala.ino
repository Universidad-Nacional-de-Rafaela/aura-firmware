/*
 * AURA - nodo de sala (XIAO ESP32S3), trama v2 / contrato v3.0
 *
 * Relevo entre los nodos de la habitacion y el gateway de la mesh. La regla que
 * evita logica por caso: si mac_destino es la propia, procesar; si no, reenviar
 * sin interpretar el contenido. Lo que sube (telemetria, resultado, reporte,
 * alerta) va al gateway; lo que baja (comando, confirmacion) va al hijo cuya
 * MAC es el destino.
 *
 * No guarda nada en flash: la durabilidad la da la cola del nodo de origen,
 * que reenvia cada muestra hasta que AURA la confirma. Si la sala se reinicia
 * con tramas en RAM, el nodo las vuelve a mandar.
 *
 * Manda su propio reporte al arrancar, al volver el enlace y cada
 * INTERVALO_REPORTE_S: con eso el gateway sabe si la sala se cayo.
 *
 * Las MAC van en config_local.h (ver config_local.h.example).
 *
 * SIN BIBLIOTECAS EXTERNAS.
 * IDE: Placa "XIAO_ESP32S3". USB CDC On Boot: ENABLED, si no el monitor
 * serie no muestra nada.
 */

#if __has_include("config_local.h")
#include "config_local.h"
#endif

#include "../../comun/radio_mesh.h"
#include "../../comun/buffer_circular.h"

// Valores por defecto si no hay config_local.h. En cero = sin configurar.
#ifndef MAC_GATEWAY
#define MAC_GATEWAY  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#endif
#ifndef MAC_ESPERADA
#define MAC_ESPERADA {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#endif
#ifndef HIJOS
#define HIJOS { {0x00, 0x00, 0x00, 0x00, 0x00, 0x00} }
#endif
// MODO_BANCO en 1: probar nodo + sala SIN el gateway encendido. No sube nada
// y no barre canales: sin gateway no hay ACK, y al minuto la sala se pondria
// a saltar de canal, rompiendo el enlace con el nodo que si funciona.
#ifndef MODO_BANCO
#define MODO_BANCO 0
#endif

const uint8_t mac_gateway[6]  = MAC_GATEWAY;
const uint8_t mac_esperada[6] = MAC_ESPERADA;
const uint8_t hijos[][6]      = HIJOS;
const int     HIJOS_N         = sizeof(hijos) / sizeof(hijos[0]);

const unsigned long SIN_ACK_MAX         = 60000;
const uint32_t      INTERVALO_REPORTE_S = 300;
const uint8_t       INTENTOS_BAJADA     = 3;

BufferCircular hacia_arriba;
BufferCircular hacia_abajo;
uint8_t  intentos_bajada = 0;
uint16_t seq_actual      = 0;
uint8_t  fallos_seguidos = 0;
unsigned long ultimo_ack_ok  = 0;
unsigned long ultimo_reporte = 0;
uint32_t reenviadas_arriba = 0, reenviadas_abajo = 0, perdidas_abajo = 0, ajenas = 0;

bool es_hijo(const uint8_t mac[6]) {
  for (int i = 0; i < HIJOS_N; i++)
    if (!aura_mac_vacia(hijos[i]) && memcmp(hijos[i], mac, 6) == 0) return true;
  return false;
}

// ===== Reporte propio =====
// Sin ArduinoJson: el JSON es fijo y chico. intervalo_s es el del reporte: es
// lo que el gateway usa para inferir offline (contrato §3.2).
void reportar() {
  char json[AURA_PAYLOAD_MAX + 1];
  int n = snprintf(json, sizeof(json),
                   "{\"config\":{\"intervalo_s\":%lu},\"pendientes\":%u,\"descartadas\":%lu,"
                   "\"alimentacion\":\"red\"}",
                   (unsigned long)INTERVALO_REPORTE_S, buffer_cantidad(&hacia_arriba),
                   (unsigned long)buffer_descartados(&hacia_arriba));
  TramaAura t;
  aura_trama_init(&t, AURA_TIPO_REPORTE, radio_mi_mac(), mac_gateway, seq_actual++,
                  (const uint8_t*)json, (uint8_t)n);
  buffer_push(&hacia_arriba, &t);
  ultimo_reporte = millis();
}

// ===== Recepcion =====
void procesar_recibidas() {
  RecibidaAura r;
  char de[18], dest[18];
  while (radio_recibir(&r)) {
    const TramaAura* t = &r.t;

    if (aura_es_para_mi(t, radio_mi_mac())) {
      radio_mandar_ack(r.de, t->seq);   // al salto anterior, siempre
      if (t->tipo == AURA_TIPO_PING) continue;
      if (t->tipo == AURA_TIPO_COMANDO) {
        // La sala no tiene comandos: el gateway no deberia mandarle ninguno.
        char json[96];
        int n = snprintf(json, sizeof(json), "{\"aplicado\":false,\"motivo\":\"comando_desconocido\"}");
        TramaAura res;
        aura_trama_init(&res, AURA_TIPO_RESULTADO, radio_mi_mac(), mac_gateway, seq_actual++,
                        (const uint8_t*)json, (uint8_t)n);
        buffer_push(&hacia_arriba, &res);
      }
      continue;
    }

    if (aura_tipo_sube(t->tipo)) {
      // Se confirma el salto apenas se acepta: desde aca se hace cargo esta placa.
      radio_mandar_ack(r.de, t->seq);
      buffer_push(&hacia_arriba, t);
      Serial.printf("[SUBE] RX tipo %u de %s seq=%u\n", t->tipo, aura_mac_texto(t->mac_origen, de), t->seq);
    } else if (aura_tipo_baja(t->tipo) && es_hijo(t->mac_destino)) {
      radio_mandar_ack(r.de, t->seq);
      buffer_push(&hacia_abajo, t);
      Serial.printf("[BAJA] RX tipo %u para %s seq=%u\n", t->tipo, aura_mac_texto(t->mac_destino, dest), t->seq);
    } else {
      // Ni para mi ni para un hijo mio: no se confirma, para no tragarla.
      ajenas++;
    }
  }
}

// ===== Envio =====
bool al_gateway(const TramaAura* t) {
  if (radio_enviar_con_ack(mac_gateway, t)) {
    ultimo_ack_ok = millis();
    if (fallos_seguidos >= 3) reportar();   // volvio el enlace: el gateway sabe que estoy
    fallos_seguidos = 0;
    return true;
  }
  if (fallos_seguidos < 255) fallos_seguidos++;
  return false;
}

void bajar_uno() {
  TramaAura t;
  if (!buffer_peek(&hacia_abajo, &t)) return;
  char m[18];
  if (radio_enviar_con_ack(t.mac_destino, &t)) {
    reenviadas_abajo++;
    Serial.printf("[BAJA] OK tipo %u -> %s seq=%u\n", t.tipo, aura_mac_texto(t.mac_destino, m), t.seq);
  } else if (++intentos_bajada < INTENTOS_BAJADA) {
    return;
  } else {
    // El nodo reintenta su muestra si no le llega la confirmacion, y el
    // gateway publica rechazado si el comando no tiene resultado: no se insiste.
    perdidas_abajo++;
    Serial.printf("[BAJA] !! %s no confirmo tipo %u seq=%u, se descarta\n",
                  aura_mac_texto(t.mac_destino, m), t.tipo, t.seq);
  }
  buffer_pop(&hacia_abajo, &t);
  intentos_bajada = 0;
}

void subir_uno() {
  TramaAura t;
  if (!buffer_peek(&hacia_arriba, &t)) return;
  if (MODO_BANCO) {
    buffer_pop(&hacia_arriba, &t);   // sin gateway: se ve en el monitor y se descarta
    return;
  }
  if (al_gateway(&t)) {
    buffer_pop(&hacia_arriba, &t);
    reenviadas_arriba++;
  } else {
    Serial.printf("[SUBE] !! el gateway no confirmo seq=%u, se reintenta\n", t.seq);
    delay(500);
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  buffer_init(&hacia_arriba);
  buffer_init(&hacia_abajo);

  if (!radio_iniciar(16)) {
    Serial.println("fallo ESP-NOW, reinicio");
    delay(2000);
    ESP.restart();
  }
  radio_verificar_placa(mac_esperada);
  radio_agregar_peer(mac_gateway);
  for (int i = 0; i < HIJOS_N; i++) radio_agregar_peer(hijos[i]);
  ultimo_ack_ok = millis();

  char m[18];
  if (MODO_BANCO)
    Serial.println("*** MODO_BANCO=1: no se sube al gateway ni se barren canales. ***");
  else if (aura_mac_vacia(mac_gateway))
    Serial.println("AVISO: MAC_GATEWAY sin configurar -> no se sube nada.");
  Serial.printf("nodo_sala listo, mi MAC %s, %d hijo(s)\n", aura_mac_texto(radio_mi_mac(), m), HIJOS_N);
  reportar();
}

// ===== LOOP =====
void loop() {
  procesar_recibidas();
  bajar_uno();            // lo que baja primero: un comando espera a una persona
  procesar_recibidas();

  if (!aura_mac_vacia(mac_gateway)) {
    subir_uno();
    // Solo se barre si hay algo SIN CONFIRMAR: estando ociosa, ultimo_ack_ok no
    // se refresca y el barrido romperia un enlace sano.
    if (!MODO_BANCO && buffer_cantidad(&hacia_arriba) > 0 && millis() - ultimo_ack_ok > SIN_ACK_MAX) {
      if (radio_barrer_canales(mac_gateway, seq_actual++)) reportar();
      ultimo_ack_ok = millis();
    }
    if (millis() - ultimo_reporte > INTERVALO_REPORTE_S * 1000UL) reportar();
  }

  static unsigned long ultimo_print = 0;
  if (millis() - ultimo_print > 10000) {
    ultimo_print = millis();
    Serial.printf("arriba: en cola %u, reenviadas %lu, descartadas %lu | abajo: reenviadas %lu, "
                  "perdidas %lu | ajenas %lu, version_distinta %lu, desbordes %lu\n",
                  buffer_cantidad(&hacia_arriba), (unsigned long)reenviadas_arriba,
                  (unsigned long)buffer_descartados(&hacia_arriba), (unsigned long)reenviadas_abajo,
                  (unsigned long)perdidas_abajo, (unsigned long)ajenas,
                  (unsigned long)radio_aura.version_distinta, (unsigned long)radio_aura.desbordes);
  }

  delay(10);
}
