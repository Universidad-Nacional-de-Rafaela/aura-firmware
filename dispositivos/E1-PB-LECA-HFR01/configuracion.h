#ifndef ICIV_NODO_CONFIGURACION_H
#define ICIV_NODO_CONFIGURACION_H

// Pines, canal y ajustes de hardware; config_local.h tiene prioridad.

#include <Arduino.h>
#if __has_include("config_local.h")
#include "config_local.h"
#endif
#ifndef ICIV_PARENT_MAC
#define ICIV_PARENT_MAC {0,0,0,0,0,0}
#endif
#ifndef ICIV_GATEWAY_MAC
#define ICIV_GATEWAY_MAC {0,0,0,0,0,0}
#endif
#ifndef ICIV_CHANNEL
#define ICIV_CHANNEL 1
#endif
#ifndef ICIV_FRIDGE_PIN
#define ICIV_FRIDGE_PIN D2
#endif
#ifndef ICIV_FREEZER_PIN
#define ICIV_FREEZER_PIN D3
#endif
#ifndef ICIV_ALARM_LED_PIN
#define ICIV_ALARM_LED_PIN D0
#endif
#ifndef ICIV_OFFLINE_LED_PIN
#define ICIV_OFFLINE_LED_PIN D1
#endif
#ifndef ICIV_POWER_PIN
#define ICIV_POWER_PIN -1
#endif
#ifndef ICIV_POWER_PRESENT_LEVEL
#define ICIV_POWER_PRESENT_LEVEL HIGH
#endif
#ifndef ICIV_POWER_PIN_MODE
#define ICIV_POWER_PIN_MODE INPUT
#endif
#ifndef ICIV_SET_RTC_FROM_BUILD
#define ICIV_SET_RTC_FROM_BUILD 0
#endif
#ifndef ICIV_RTC_UTC_OFFSET_SECONDS
#define ICIV_RTC_UTC_OFFSET_SECONDS 0
#endif

#endif
