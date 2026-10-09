// RTC DS3231: hora de medicion UTC y validacion del reloj.

#ifndef ICIV_NODO_RELOJ_H
#define ICIV_NODO_RELOJ_H

#include "estado_nodo.h"

void beginClock() {
  rtcOk=rtc.begin();
  if(rtcOk && rtc.lostPower() && ICIV_SET_RTC_FROM_BUILD)rtc.adjust(DateTime(F(__DATE__),F(__TIME__)));
  clockOk=rtcOk && !rtc.lostPower();
}


uint32_t readEpoch() {
  if(rtcOk && !rtc.lostPower()) {
    DateTime t=rtc.now();
    int64_t utc=int64_t(t.unixtime())-ICIV_RTC_UTC_OFFSET_SECONDS;
    if(t.isValid() && utc>=AURA_EPOCA_MINIMA && utc<=UINT32_MAX) { clockOk=true; return uint32_t(utc); }
  }
  uint32_t epoch=nodo_mesh_ahora(); clockOk=epoch!=0; return epoch;
}
void seedSystemClock() {
  uint32_t epoch=readEpoch();
  if(epoch) { timeval tv={time_t(epoch),0}; settimeofday(&tv,nullptr); }
}
#endif
