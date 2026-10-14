#pragma once
// El programa coordina los modulos; la red es una hoja ESP-WIFI-MESH de AURA.
#include "sondas.h"
#include "energia.h"
#include "comunicacion.h"

void setup() {
  Serial.begin(115200);
  beginOutputs(); beginPower(); beginDisplay(); beginClock();
  beginProbes(); beginStorage(); seedSystemClock(); beginNetwork();
  scheduleFirstMeasurements(); updateOutputs();
  Serial.printf("[NODO] AURA v4; NVS=%s; heladera=%lu s, freezer=%lu s\n",
    storageOk?"OK":"ERROR",(unsigned long)journal.config.sensors[0].intervalS,
    (unsigned long)journal.config.sensors[1].intervalS);
}
void loop() {
  uint32_t now=millis();
  readPower(now); beginMeasurements(now); finishMeasurements(now);
  serviceNetwork(now); drawDisplay(now); delay(5);
}
