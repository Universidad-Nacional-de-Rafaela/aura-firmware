/*
 * AURA - relevo de la mesh (XIAO ESP32S3), contrato v4.0, ESP-WIFI-MESH
 *
 * Placa de la catedra que extiende la mesh: se une como nodo intermedio y
 * reenvia el trafico de las hojas (los dispositivos de los grupos) hacia el
 * raiz. No tiene logica de AURA: ESP-WIFI-MESH arma las rutas sola, asi que
 * no hay lista de hijos ni MAC del raiz que configurar. Si un relevo se cae,
 * sus hojas se enganchan a otro padre si lo tienen a su alcance.
 *
 * Reemplaza a nodo_sala (mesh ESP-NOW, tag mesh-espnow-v2).
 *
 * Configuracion en config_local.h (ver config_local.h.example): ID, clave y
 * canal de la mesh. No lleva la clave del WiFi del edificio.
 *
 * IDE: Placa "XIAO_ESP32S3". USB CDC On Boot: ENABLED.
 */

#if __has_include("config_local.h")
#include "config_local.h"
#endif

#include "../../comun/radio_wifi_mesh.h"

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

const uint8_t mesh_id[6]      = MESH_ID;
const uint8_t mac_esperada[6] = MAC_ESPERADA;

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (aura_mac_vacia(mesh_id)) {
    Serial.println("nodo_relevo: MESH_ID sin configurar (config_local.h), no hace nada");
    return;
  }
  AuraMeshConfig mc = {{0}, MESH_CLAVE, MESH_CANAL, MESH_ROUTER_SSID, ""};
  memcpy(mc.mesh_id, mesh_id, 6);
  if (!radio_iniciar(AURA_ROL_RELEVO, &mc, 4)) {
    Serial.println("no arranco la mesh, reinicio");
    delay(2000);
    ESP.restart();
  }
  radio_verificar_placa(mac_esperada);
}

void loop() {
  // El relevo no consume nada: lo que le llega para si mismo (nada, en
  // principio) se descarta para que no se llene la cola.
  RecibidaAura r;
  while (radio_recibir(&r)) {}

  static unsigned long ultimo = 0;
  if (millis() - ultimo > 30000) {
    ultimo = millis();
    Serial.printf("[RELEVO] %s, capa %d, nodos en la mesh %d, descartadas: version %lu, invalidas %lu\n",
                  radio_conectada() ? "unido" : "SIN PADRE", radio_capa(), radio_nodos_en_mesh(),
                  (unsigned long)radio_aura.version_distinta, (unsigned long)radio_aura.invalidas);
  }
  delay(50);
}
