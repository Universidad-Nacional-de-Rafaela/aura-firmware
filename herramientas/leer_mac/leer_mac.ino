/*
 * AURA - utilidad: leer la MAC de una placa
 *
 * Se flashea en cada XIAO ESP32S3 antes de configurarla, para saber:
 *   - su hw_id en AURA ("mac-" + la MAC en minusculas): con eso la catedra
 *     asigna la placa a su dispositivo (contrato v4.0, identidad por placa);
 *   - el valor de MAC_ESPERADA para su config_local.h.
 *
 * Imprime la MAC de la interfaz STA, que es la que usa ESP-WIFI-MESH como
 * direccion del nodo.
 */

#include <WiFi.h>
#include <esp_wifi.h>

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);

  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);

  Serial.println();
  Serial.println("=== MAC de esta placa ===");
  Serial.printf("hw_id en AURA:              mac-%02x%02x%02x%02x%02x%02x\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("MAC_ESPERADA (config_local): {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println();
  Serial.println("Anotala y pegale una etiqueta a la placa con su hw_id.");
}

void loop() {
  delay(10000);
}
