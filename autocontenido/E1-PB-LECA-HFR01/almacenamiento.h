#pragma once
#include "estado_nodo.h"
void beginStorage() {
  storageOk=store.begin(journal);
  if (!storageOk) journalInit(journal);
  if (storageOk) storageOk=flightStore.begin(flight);
  needsFirst=journal.capturePending; cutEpoch=journal.cutAt;
  for(int s=0;s<2;++s) sensorKnown[s]=journal.sensorKnown[s];
  battery=ICIV_POWER_PIN>=0 && journal.powerKnown && journal.onBattery;
}
bool commit() {
  if(storageOk && store.save(journal)) return true;
  storageOk=false; linkState=GATEWAY_DOWN;
  Serial.println("[NVS] Fallo: se detiene el envio; no se borran datos sin guardar");
  return false;
}
void newId(uint8_t id[16]) { aura_ingest_id_nuevo(id,nodo_mesh_azar); }
