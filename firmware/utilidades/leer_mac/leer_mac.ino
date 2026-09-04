/*
 * AURA - utilidad: leer la MAC de una placa
 *
 * Se flashea en cada ESP32 ANTES de configurar la mesh, para saber que MAC
 * poner en MAC_PADRE, MAC_SENSOR, MAC_GATEWAY y en la TABLA del gateway.
 *
 * Imprime la MAC de la interfaz STA, que es la que usa ESP-NOW y la misma
 * que hay que cargar en el campo mac_address del dispositivo en AURA.
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
  Serial.printf("Para AURA (mac_address):  %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("Para el firmware (C):     {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println();
  Serial.println("Anotala y pegale una etiqueta a la placa: SENSOR, SALA o GATEWAY.");
}

void loop() {
  delay(10000);
}
