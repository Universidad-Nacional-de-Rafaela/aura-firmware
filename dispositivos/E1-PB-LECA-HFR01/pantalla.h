// pantalla OLED

#ifndef ICIV_NODO_PANTALLA_H
#define ICIV_NODO_PANTALLA_H

// OLED y LEDs: temperaturas, alarmas locales y estado de conexion.
#include "estado_nodo.h"

void beginOutputs() {
  pinMode(ICIV_ALARM_LED_PIN,OUTPUT);pinMode(ICIV_OFFLINE_LED_PIN,OUTPUT);
}
void beginDisplay() {
  Wire.begin(D4,D5);oled.begin();
}

bool outside(int s) {
  return isfinite(latest[s]) && (latest[s]*100<journal.config.sensors[s].min100 || latest[s]*100>journal.config.sensors[s].max100);
}

void updateOutputs() {
  digitalWrite(ICIV_ALARM_LED_PIN,outside(0)||outside(1)?HIGH:LOW);
  digitalWrite(ICIV_OFFLINE_LED_PIN,linkState==GATEWAY_DOWN||linkState==CENTRAL_DOWN||!storageOk?HIGH:LOW);
  // Bateria y caida de comunicacion son estados independientes.
  oled.setPowerSave(battery||linkState==GATEWAY_DOWN||linkState==CENTRAL_DOWN);
}

void drawDisplay(uint32_t now) {
  if(uint32_t(now-displayAt)<1000 || battery || linkState==GATEWAY_DOWN || linkState==CENTRAL_DOWN)return;
  displayAt=now;oled.clearBuffer();oled.setFont(u8g2_font_6x10_tf);
  char text[28];
  for(int s=0;s<2;++s) {
    if(isfinite(latest[s]))snprintf(text,sizeof(text),"%s %.2f C",s?"Freezer:":"Heladera:",latest[s]);
    else snprintf(text,sizeof(text),"%s ERROR SONDA",s?"Freezer":"Heladera");
    oled.drawStr(0,12+s*12,text);
  }
  snprintf(text,sizeof(text),"Cola H:%u F:%u",journal.count[0],journal.count[1]);oled.drawStr(0,39,text);
  oled.drawStr(0,51,clockOk?"RTC valido":"RTC SIN HORA VALIDA");
  oled.drawStr(0,63,!storageOk?"ERROR NVS":!macSet(parentMac)?"CONFIGURAR MAC":linkState==ONLINE?"Gateway + AURA OK":"Esperando ACK finales");oled.sendBuffer();
}

#endif
