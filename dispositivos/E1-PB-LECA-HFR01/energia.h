#pragma once
#include "almacenamiento.h"
#include "reloj.h"
#include "pantalla.h"
void beginPower() { if(ICIV_POWER_PIN>=0) pinMode(ICIV_POWER_PIN,ICIV_POWER_PIN_MODE); }
void readPower(uint32_t now) {
  if(ICIV_POWER_PIN<0) return;
  bool observed=digitalRead(ICIV_POWER_PIN)!=ICIV_POWER_PRESENT_LEVEL;
  if(!powerSeen || observed!=powerCandidate) { powerSeen=true; powerCandidate=observed; powerChangedAt=now; }
  if(uint32_t(now-powerChangedAt)<200) return;
  if(!journal.powerKnown || observed!=bool(journal.onBattery)) {
    bool entered=observed && (!journal.powerKnown || !journal.onBattery);
    journal.powerKnown=1; journal.onBattery=observed; battery=observed;
    if(entered) {
      cutEpoch=readEpoch(); journal.cutAt=cutEpoch; journal.capturePending=1; needsFirst=true;
      // El inicio del corte se conserva localmente incluso con las dos sondas ausentes.
      // No es una medicion de temperatura ni tiene ACK de telemetria.
      // journal.cutAt y capturePending se guardan aun sin lecturas validas.
      Serial.printf("[ENERGIA] Inicio del corte UTC=%lu; capturar ambas sondas\n",(unsigned long)cutEpoch);
    }
    if(storageOk) commit();
    if(radioOk) nodo_mesh_alimentacion(observed?"bateria":"red");
    snapshotPending=true; updateOutputs();
  }
}
