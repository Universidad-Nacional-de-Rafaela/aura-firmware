#pragma once
/* Propuesta de cableado para ESP32-C3 SuperMini: confirmar en el banco.
 * Los GPIO se expresan con su numero real, no con los D0..D5 de la XIAO.
 * No usar GPIO18/19 (USB) ni GPIO2/8/9 (arranque) para este cableado. */
#define HFR_PIN_HELADERA 3
#define HFR_PIN_FREEZER 4
#define HFR_PIN_SDA 5
#define HFR_PIN_SCL 6
#define HFR_PIN_LED_UMBRAL 7
#define HFR_PIN_LED_RED 10
#define HFR_LED_ACTIVO 1
#ifndef HFR_PIN_ALIMENTACION
#define HFR_PIN_ALIMENTACION (-1)
#endif
#define HFR_NIVEL_RED 1
#define HFR_POWER_PULLUP 0
#define HFR_OLED_DIRECCION 0x3c
#define HFR_RTC_DIRECCION 0x68
#define HFR_RTC_OFFSET_S 0
#define HFR_POTENCIA_TX 60
#define HFR_OSCURO_TRAS_MS 15000
