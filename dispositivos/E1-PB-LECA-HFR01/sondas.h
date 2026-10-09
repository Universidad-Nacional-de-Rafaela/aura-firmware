#pragma once
#include "almacenamiento.h"
#include "reloj.h"
#include "pantalla.h"
void beginProbes() {
  for(int s=0;s<2;++s) { probes[s]->begin(); probes[s]->setResolution(9); probes[s]->setWaitForConversion(false); }
}
void scheduleFirstMeasurements() {
  for(int s=0;s<2;++s) lastMeasure[s]=millis()-journal.config.sensors[s].intervalS*1000UL;
}
void beginMeasurements(uint32_t now) {
  if(conversion) return;
  uint8_t mask=needsFirst?3:0;
  for(int s=0;s<2;++s) if(uint32_t(now-lastMeasure[s])>=journal.config.sensors[s].intervalS*1000UL) mask|=1<<s;
  if(!mask) return;
  dueMask=mask; captureCut=needsFirst; needsFirst=false;
  conversionEpoch=captureCut?cutEpoch:readEpoch(); sampleClock=conversionEpoch!=0;
  for(int s=0;s<2;++s) if(mask&(1<<s)) { probes[s]->requestTemperatures(); lastMeasure[s]=now; }
  conversion=true; conversionStart=now;
}
void finishMeasurements(uint32_t now) {
  if(!conversion || uint32_t(now-conversionStart)<100) return;
  const char* campos[2]={"temp_heladera_c","temp_freezer_c"};
  for(int s=0;s<2;++s) if(dueMask&(1<<s)) {
    bool ready=probes[s]->isConversionComplete();
    float t=ready?probes[s]->getTempCByIndex(0):NAN;
    uint8_t state=classifyTemperature(t,ready);
    latest[s]=state==SONDA_OK?t:NAN; sensorKnown[s]=true;
    journal.sensorKnown[s]=1; journal.sensorState[s]=state;
    if(radioOk) nodo_mesh_sonda(campos[s],state==SONDA_OK,state==FUERA_RANGO?"fuera_de_rango":"sin_respuesta");
    if(state==SONDA_OK) {
      Muestra m={}; newId(m.id); m.sensor=s; m.measuredAt=conversionEpoch; m.cutAt=journal.cutAt;
      m.temp100=int16_t(lround(t*100));
      m.flags=VALID|(battery?BATTERY:0)|(captureCut?POWER_FIRST:0)|(sampleClock?CLOCK_VALID:0)|
        ((ICIV_POWER_PIN>=0 && journal.powerKnown)?POWER_KNOWN:0);
      if(!captureCut || !protectFirst(journal,m)) pushSample(journal,m);
    }
    Serial.printf("[MUESTRA] %s %.2f C estado=%u t=%lu pendientes=%u\n",s?"freezer":"heladera",latest[s],state,
      (unsigned long)conversionEpoch,journal.count[s]);
  }
  if(captureCut) journal.capturePending=0;
  conversion=false; snapshotPending=true;
  if(storageOk) commit();
  updateOutputs();
}
