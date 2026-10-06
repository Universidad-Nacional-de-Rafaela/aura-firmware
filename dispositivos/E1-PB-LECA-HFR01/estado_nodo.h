#ifndef ICIV_NODO_ESTADO_NODO_H
#define ICIV_NODO_ESTADO_NODO_H

// Objetos y estado compartido del nodo. Se incluye desde un solo sketch.
#include "configuracion.h"
#include "mesh_radio.h"
#include "mesh_storage.h"

#include <OneWire.h>
#include <DallasTemperature.h>
#include <U8g2lib.h>
#include <RTClib.h>
#include <Wire.h>

const uint8_t parentMac[6]=ICIV_PARENT_MAC, gatewayMac[6]=ICIV_GATEWAY_MAC;
OneWire wireFridge(ICIV_FRIDGE_PIN), wireFreezer(ICIV_FREEZER_PIN);
DallasTemperature fridge(&wireFridge), freezer(&wireFreezer);
DallasTemperature* probes[2]={&fridge,&freezer};
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0,U8X8_PIN_NONE);
RTC_DS3231 rtc;
bool rtcOk=false,clockOk=false,storageOk=false,radioOk=false,radioRunning=false;
Journal journal;
MeshStorage store;
float latest[2]={NAN,NAN};
bool sensorKnown[2]={false,false},snapshotPending=true,forceSnapshot=true;
uint32_t lastRadioSend=0;
uint32_t lastMeasure[2]={0,0}, conversionStart=0,conversionEpoch=0;
uint32_t displayAt=0, nextRecovery=0, cutEpoch=0;
uint8_t dueMask=0;
bool conversion=false, captureCut=false, needsFirst=false, sampleClock=false;
bool battery=false,powerCandidate=false,powerSeen=false;
uint32_t powerChangedAt=0;
enum LinkState {UNKNOWN,ONLINE,GATEWAY_DOWN,CENTRAL_DOWN};
LinkState linkState=UNKNOWN;
Muestra flight;
bool waiting=false, gatewayReceived=false,everGateway=false,scanning=false;
uint8_t attempts=0,scanChannel=1;
uint8_t listeningChannel=ICIV_CHANNEL;
uint32_t sentAt=0,scanAt=0;
uint16_t seq=0;

#endif
