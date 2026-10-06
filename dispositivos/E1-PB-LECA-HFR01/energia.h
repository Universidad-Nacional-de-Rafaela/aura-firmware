// modo de bateria

#ifndef ICIV_NODO_ENERGIA_H
#define ICIV_NODO_ENERGIA_H

// Entrada de alimentacion, antirrebote y registro protegido del corte.
#include "almacenamiento.h"
#include "reloj.h"
#include "pantalla.h"

void beginPower() {
  if(ICIV_POWER_PIN>=0)pinMode(ICIV_POWER_PIN,ICIV_POWER_PIN_MODE);
}

void readPower(uint32_t now) {
  if(ICIV_POWER_PIN<0)return;
  const bool observed=digitalRead(ICIV_POWER_PIN)!=ICIV_POWER_PRESENT_LEVEL;
  if(!powerSeen || observed!=powerCandidate) {
    powerSeen=true;powerCandidate=observed;powerChangedAt=now;
  }
  if(uint32_t(now-powerChangedAt)<200)return; // antirrebote de entrada
  if(!journal.powerKnown || observed!=bool(journal.onBattery)) {
    const bool transitioned=!journal.powerKnown || !journal.onBattery;
    journal.powerKnown=1;journal.onBattery=observed;battery=observed;snapshotPending=true;
    Muestra event={};newId(event.id);event.kind=ALERTA_ENERGIA;event.state=observed;
    event.sensor=0;event.temp100=TEMP_INVALIDA;event.measuredAt=readEpoch();
    event.flags=POWER_KNOWN|(observed?BATTERY:0)|(event.measuredAt?CLOCK_VALID:0);
    if(observed && (!journal.hasPower || journal.powerAcked)) {journal.firstPower=event;journal.hasPower=1;journal.powerAcked=0;}
    else pushSample(journal,event);
    if(observed && transitioned) {
      cutEpoch=readEpoch();journal.cutAt=cutEpoch;journal.capturePending=1;needsFirst=true;
      Serial.println("[ENERGIA] Bateria: capturar ambas sondas; hora = deteccion del corte");
    }
    if(storageOk)commit();updateOutputs();
  }
}

#endif
