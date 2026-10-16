#pragma once
// Radio ESP-WIFI-MESH compartida por los tres roles de la mesh de AURA (raiz,
// relevo, hoja). Solo compila en la placa. Es una capa fina sobre la API
// esp_mesh de ESP-IDF, que el core de Arduino trae compilada: la logica que se
// puede probar sin placa vive en protocolo_aura.h y en los *_logica.h.
//
//   raiz   (catedra, una por edificio): MESH_ROOT fijo. Es el unico que se
//          asocia al router (WiFi del edificio) y toma IP. Es el gateway.
//   relevo (catedra): MESH_NODE. Reenvia; no tiene logica de AURA.
//   hoja   (cada dispositivo de un grupo): MESH_LEAF. No reenvia nada: un
//          grupo que reflashea o tiene un bug no corta a los demas.
//
// Reglas:
//   1. La identidad es la MAC de fabrica de la estacion (esp_read_mac STA): es
//      la que la mesh usa como direccion y la que el raiz ve como origen.
//   2. La tarea de recepcion solo valida y encola: nunca envia ni bloquea el loop.
//   3. Todos los nodos se configuran con raiz fijo: ninguno intenta ser raiz.
//   4. Cada sketch declara en que placa debe correr y lo grita si no coincide.
//
// SIN PROBAR EN PLACA (2026-10-07): la secuencia de arranque (DHCP solo en el
// raiz) y si una hoja se une sin la clave del router. Ver docs/mesh-wifi/.

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_mesh.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "protocolo_aura.h"

typedef enum { AURA_ROL_RAIZ = 0, AURA_ROL_RELEVO, AURA_ROL_HOJA } AuraRol;

// Lo que cada sketch saca de su config_local.h. router_ssid y router_clave
// solo los necesita el raiz: las hojas y los relevos los dejan vacios.
typedef struct {
  uint8_t     mesh_id[6];
  const char* clave;          // clave de la mesh (WPA2, 8 a 63 caracteres)
  uint8_t     canal;          // canal del router; 0 = buscar en todos
  const char* router_ssid;
  const char* router_clave;
} AuraMeshConfig;

// Una trama recibida y la MAC de quien la origino (no del ultimo salto).
typedef struct {
  TramaAura t;
  uint8_t   de[6];
} RecibidaAura;

typedef struct {
  AuraRol           rol;
  uint8_t           mi_mac[6];
  QueueHandle_t     cola;
  esp_netif_t*      sta;
  volatile bool     padre_ok;   // hoja/relevo: unido a la mesh; raiz: asociado al router
  volatile bool     ip_ok;      // solo el raiz
  volatile bool     reconecto;  // paso de desconectado a conectado; lo consume quien llama
  volatile int      capa;
  // Lo que se descarta en la recepcion, contado para que no sea silencioso.
  volatile uint32_t version_distinta;
  volatile uint32_t invalidas;
  volatile uint32_t desbordes;
} RadioAura;

static RadioAura radio_aura;

#define AURA_MESH_ENVIO_MS 3000   // tope de esp_mesh_send: no bloquear el loop para siempre

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

static inline const uint8_t* radio_mi_mac() { return radio_aura.mi_mac; }
static inline int radio_capa() { return radio_aura.capa; }
static inline int radio_nodos_en_mesh() { return esp_mesh_get_routing_table_size(); }

// Lista para enviar: la hoja y el relevo, unidos a la mesh; el raiz, con IP.
static inline bool radio_conectada() {
  return radio_aura.rol == AURA_ROL_RAIZ ? radio_aura.ip_ok : radio_aura.padre_ok;
}

// true una sola vez despues de cada reconexion: el que llama manda su reporte.
static inline bool radio_reconecto() {
  if (!radio_aura.reconecto) return false;
  radio_aura.reconecto = false;
  return true;
}

static void radio_evento_mesh(void*, esp_event_base_t, int32_t id, void*) {
  switch (id) {
    case MESH_EVENT_PARENT_CONNECTED:
      radio_aura.capa = esp_mesh_get_layer();
      radio_aura.padre_ok = true;
      if (radio_aura.rol != AURA_ROL_RAIZ) radio_aura.reconecto = true;
      // El raiz es el unico que pide IP al router (regla del ejemplo de ESP-IDF).
      if (radio_aura.rol == AURA_ROL_RAIZ && radio_aura.sta) esp_netif_dhcpc_start(radio_aura.sta);
      break;
    case MESH_EVENT_PARENT_DISCONNECTED:
      radio_aura.padre_ok = false;
      radio_aura.ip_ok = false;
      break;
    default:
      break;
  }
}

static void radio_evento_ip(void*, esp_event_base_t, int32_t id, void*) {
  if (id == IP_EVENT_STA_GOT_IP) {
    radio_aura.ip_ok = true;
    radio_aura.reconecto = true;
  } else if (id == IP_EVENT_STA_LOST_IP) {
    radio_aura.ip_ok = false;
  }
}

static void radio_tarea_rx(void*) {
  static uint8_t buf[MESH_MPS];
  for (;;) {
    mesh_addr_t de;
    mesh_data_t d;
    d.data = buf;
    d.size = sizeof(buf);
    int flag = 0;
    if (esp_mesh_recv(&de, &d, portMAX_DELAY, &flag, NULL, 0) != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    // "x = x + 1" y no "x++": ++ sobre volatile esta deprecado en C++20.
    AuraValidacion v = aura_trama_validar(buf, d.size);
    if (v == AURA_TRAMA_VERSION) { radio_aura.version_distinta = radio_aura.version_distinta + 1; continue; }
    if (v != AURA_TRAMA_OK)      { radio_aura.invalidas = radio_aura.invalidas + 1; continue; }
    RecibidaAura r;
    memset(&r, 0, sizeof(r));
    memcpy(&r.t, buf, d.size);
    memcpy(r.de, de.addr, 6);
    if (xQueueSend(radio_aura.cola, &r, 0) != pdTRUE) radio_aura.desbordes = radio_aura.desbordes + 1;
  }
}

static inline bool radio_recibir(RecibidaAura* r) {
  return radio_aura.cola && xQueueReceive(radio_aura.cola, r, 0) == pdTRUE;
}

static inline bool radio_enviar_a(const mesh_addr_t* destino, const TramaAura* t) {
  mesh_data_t d;
  d.data  = (uint8_t*)t;
  d.size  = aura_trama_bytes(t);
  d.proto = MESH_PROTO_BIN;
  d.tos   = MESH_TOS_P2P;   // con reintentos salto a salto
  return esp_mesh_send(destino, &d, MESH_DATA_P2P, NULL, 0) == ESP_OK;
}

// Hoja o relevo -> raiz. El destino NULL es "el raiz", sea cual sea su MAC:
// cambiar la placa del raiz no obliga a reflashear ningun nodo.
static inline bool radio_al_raiz(const TramaAura* t) {
  if (!radio_aura.padre_ok) return false;
  return radio_enviar_a(NULL, t);
}

// Raiz -> un nodo. La mesh enruta: no hace falta saber por que relevo cuelga.
static inline bool radio_a(const uint8_t mac[6], const TramaAura* t) {
  mesh_addr_t destino;
  memcpy(destino.addr, mac, 6);
  return radio_enviar_a(&destino, t);
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

static inline const char* radio_rol_texto(AuraRol rol) {
  return rol == AURA_ROL_RAIZ ? "raiz" : rol == AURA_ROL_RELEVO ? "relevo" : "hoja";
}

#define AURA_ERR(x) do { esp_err_t e_ = (x); if (e_ != ESP_OK) { \
  Serial.printf("[MESH] !! %s -> %s\n", #x, esp_err_to_name(e_)); return false; } } while (0)

static inline bool radio_iniciar(AuraRol rol, const AuraMeshConfig* cfg, uint8_t capacidad_cola) {
  memset(&radio_aura, 0, sizeof(radio_aura));
  radio_aura.rol = rol;

  // esp_mesh_set_config rechaza un router sin SSID (ESP_ERR_MESH_ARGUMENT, visto en placa el
  // 2026-10-09): la hoja y el relevo necesitan el SSID del edificio, aunque no su clave.
  if (!cfg->router_ssid || !cfg->router_ssid[0]) {
    Serial.println("[MESH] !! falta el SSID del WiFi del edificio (MESH_ROUTER_SSID en config_local.h)");
    return false;
  }

  // Sin esto, en placa, Arduino guarda SSID y clave en la NVS y reconecta por su cuenta al
  // desconectarse: se pelea con la mesh y genera cientos de desconexiones por segundo que
  // saturan el canal. La mesh maneja sola la conexion al padre y al router.
  WiFi.persistent(false);         // antes de WiFi.mode(): decide si el driver usa la NVS
  // WiFi.mode() inicializa netif, el loop de eventos y el driver WiFi.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  esp_read_mac(radio_aura.mi_mac, ESP_MAC_WIFI_STA);
  radio_aura.sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (radio_aura.sta) esp_netif_dhcpc_stop(radio_aura.sta);   // el raiz lo arranca al asociarse
  else Serial.println("[MESH] !! no se encontro la interfaz WIFI_STA_DEF");
  esp_wifi_set_ps(WIFI_PS_NONE);

  radio_aura.cola = xQueueCreate(capacidad_cola, sizeof(RecibidaAura));
  if (!radio_aura.cola) return false;

  AURA_ERR(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &radio_evento_mesh, NULL));
  AURA_ERR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &radio_evento_ip, NULL));
  AURA_ERR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &radio_evento_ip, NULL));

  AURA_ERR(esp_mesh_init());
  AURA_ERR(esp_mesh_set_topology(MESH_TOPO_TREE));
  AURA_ERR(esp_mesh_set_max_layer(6));
  AURA_ERR(esp_mesh_set_xon_qsize(32));
  AURA_ERR(esp_mesh_disable_ps());
  AURA_ERR(esp_mesh_set_ap_assoc_expire(10));
  AURA_ERR(esp_mesh_send_block_time(AURA_MESH_ENVIO_MS));
  AURA_ERR(esp_mesh_fix_root(true));
  if (rol == AURA_ROL_RAIZ) AURA_ERR(esp_mesh_set_type(MESH_ROOT));
  if (rol == AURA_ROL_HOJA) AURA_ERR(esp_mesh_set_type(MESH_LEAF));

  mesh_cfg_t mc = MESH_INIT_CONFIG_DEFAULT();
  memcpy(mc.mesh_id.addr, cfg->mesh_id, 6);
  mc.channel = cfg->canal;
  mc.allow_channel_switch = cfg->canal == 0;
  if (cfg->router_ssid && cfg->router_ssid[0]) {
    size_t n = strlen(cfg->router_ssid);
    if (n > sizeof(mc.router.ssid)) n = sizeof(mc.router.ssid);
    memcpy(mc.router.ssid, cfg->router_ssid, n);
    mc.router.ssid_len = (uint8_t)n;
  }
  if (cfg->router_clave)
    strncpy((char*)mc.router.password, cfg->router_clave, sizeof(mc.router.password) - 1);
  size_t largo_clave = cfg->clave ? strlen(cfg->clave) : 0;
  if (largo_clave >= 8 && largo_clave < sizeof(mc.mesh_ap.password)) {
    memcpy(mc.mesh_ap.password, cfg->clave, largo_clave);
    AURA_ERR(esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK));
  } else {
    Serial.println("[MESH] !! MESH_CLAVE vacia o de menos de 8 caracteres: la mesh queda ABIERTA");
    AURA_ERR(esp_mesh_set_ap_authmode(WIFI_AUTH_OPEN));
  }
  mc.mesh_ap.max_connection = 6;
  mc.mesh_ap.nonmesh_max_connection = 0;
  AURA_ERR(esp_mesh_set_config(&mc));
  AURA_ERR(esp_mesh_start());

  xTaskCreate(radio_tarea_rx, "aura_rx", 4096, NULL, 5, NULL);

  char m[18];
  Serial.printf("[MESH] %s iniciado, mi MAC %s, canal %u\n", radio_rol_texto(rol),
                aura_mac_texto(radio_aura.mi_mac, m), cfg->canal);
  return true;
}
