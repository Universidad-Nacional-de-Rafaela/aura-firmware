#ifndef ICIV_NODO_APP_H
#define ICIV_NODO_APP_H

// Coordinacion del nodo: cada modulo contiene su propia responsabilidad.
#include "sondas.h"
#include "energia.h"
#include "comunicacion.h"

void setup() {
  Serial.begin(115200);
  beginOutputs();
  beginPower();
  beginDisplay();
  beginClock();
  beginProbes();
  beginStorage();
  beginNetwork();
  scheduleFirstMeasurements();
  checkNetworkConfiguration();
  Serial.printf("[MESH] NVS=%s bytes=%u; MAC %02X:%02X:%02X:%02X:%02X:%02X\n",storageOk?"OK":"ERROR",unsigned(sizeof(Journal)),ownMac[0],ownMac[1],ownMac[2],ownMac[3],ownMac[4],ownMac[5]);
  updateOutputs();
}
void loop() {
  uint32_t now=millis();
  readPower(now);
  beginMeasurements(now);
  finishMeasurements(now);
  serviceNetwork(now);
  sendSnapshot(now);
  drawDisplay(now);
  delay(5);
}

#endif
