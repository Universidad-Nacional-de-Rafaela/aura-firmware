#ifndef ICIV_NODO_ALMACENAMIENTO_H
#define ICIV_NODO_ALMACENAMIENTO_H

// Persistencia de la cola y configuracion, recuperacion e IDs de eventos.
#include "estado_nodo.h"

void beginStorage() {
  storageOk=store.begin(journal);if(!storageOk)journalInit(journal);
  needsFirst=journal.capturePending;cutEpoch=journal.cutAt;
  for(int s=0;s<2;++s)sensorKnown[s]=journal.sensorKnown[s];
  battery=ICIV_POWER_PIN>=0 && journal.powerKnown && journal.onBattery;
}

bool commit() {
  if(store.save(journal))return true;
  storageOk=false;waiting=false;linkState=GATEWAY_DOWN;
  Serial.println("[NVS] Fallo: no confirmar ni enviar datos que no quedaron guardados");return false;
}

void newId(uint8_t id[16]) {esp_fill_random(id,16);id[6]=(id[6]&15)|64;id[8]=(id[8]&63)|128;}

#endif
