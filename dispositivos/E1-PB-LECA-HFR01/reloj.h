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
  if(!rtcOk || rtc.lostPower()) {clockOk=false;return 0;}
  DateTime t=rtc.now();clockOk=t.isValid() && t.unixtime()>=1700000000UL;
  return clockOk?uint32_t(int64_t(t.unixtime())-ICIV_RTC_UTC_OFFSET_SECONDS):0;
}

#endif
