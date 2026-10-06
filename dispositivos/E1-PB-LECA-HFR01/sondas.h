#ifndef ICIV_NODO_SONDAS_H
#define ICIV_NODO_SONDAS_H

// Dos DS18B20: intervalos independientes, conversion sin espera y alertas.
#include "almacenamiento.h"
#include "reloj.h"
#include "pantalla.h"

void beginProbes() {
  for(int s=0;s<2;++s) {probes[s]->begin();probes[s]->setResolution(9);probes[s]->setWaitForConversion(false);}
}
void scheduleFirstMeasurements() {
  for(int s=0;s<2;++s)lastMeasure[s]=millis()-journal.config.sensors[s].intervalS*1000UL;
}

void beginMeasurements(uint32_t now) {
  if(conversion)return;
  uint8_t mask=needsFirst?3:0;
  for(int s=0;s<2;++s)if(uint32_t(now-lastMeasure[s])>=journal.config.sensors[s].intervalS*1000UL)mask|=1<<s;
  if(!mask)return;
  dueMask=mask;captureCut=needsFirst;needsFirst=false;
  conversionEpoch=captureCut?cutEpoch:readEpoch();sampleClock=conversionEpoch!=0;
  for(int s=0;s<2;++s)if(mask&(1<<s)) {probes[s]->requestTemperatures();lastMeasure[s]=now;}
  conversion=true;conversionStart=now;
}

void finishMeasurements(uint32_t now) {
  if(!conversion || uint32_t(now-conversionStart)<100)return;
  for(int s=0;s<2;++s)if(dueMask&(1<<s)) {
    const bool ready=probes[s]->isConversionComplete();
    const float t=ready?probes[s]->getTempCByIndex(0):NAN;
    uint8_t state=classifyTemperature(t,ready);
    bool changed=!journal.sensorKnown[s] || journal.sensorState[s]!=state;
    bool recovered=journal.sensorKnown[s] && journal.sensorState[s]!=SONDA_OK && state==SONDA_OK;
    latest[s]=state==SONDA_OK?t:NAN;sensorKnown[s]=true;snapshotPending=true;
    journal.sensorKnown[s]=1;journal.sensorState[s]=state;
    Muestra m={};newId(m.id);m.sensor=s;m.measuredAt=conversionEpoch;m.cutAt=journal.cutAt;
    m.temp100=state==SONDA_OK?int16_t(lround(t*100)):TEMP_INVALIDA;
    m.flags=(state==SONDA_OK?VALID:0)|(battery?BATTERY:0)|(captureCut?POWER_FIRST:0)|(sampleClock?CLOCK_VALID:0)|((ICIV_POWER_PIN>=0&&journal.powerKnown)?POWER_KNOWN:0);
    if(state!=SONDA_OK){m.kind=ALERTA_SENSOR;m.state=state;}
    if(state==SONDA_OK || changed) {
      if(!captureCut || !protectFirst(journal,m))pushSample(journal,m);
    }
    if(recovered) {
      Muestra alert=m;newId(alert.id);alert.kind=ALERTA_SENSOR;alert.state=SONDA_OK;
      alert.flags&=~(VALID|POWER_FIRST);alert.temp100=TEMP_INVALIDA;pushSample(journal,alert);
    }
    Serial.printf("[MUESTRA] %s %.2f C estado=%u t=%lu pendientes=%u descartadas=%lu\n",s?"freezer":"heladera",latest[s],state,(unsigned long)m.measuredAt,journal.count[s],(unsigned long)journal.dropped[s]);
  }
  if(captureCut)journal.capturePending=0;
  conversion=false;if(storageOk)commit();updateOutputs();
}

#endif
