#ifndef ICIV_MESH_RADIO_H
#define ICIV_MESH_RADIO_H
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "mesh_core.h"
struct RadioPacket {TramaMesh trama; uint8_t sender[6];};
static QueueHandle_t meshRx=nullptr;
static uint8_t ownMac[6];
static void meshReceive(const esp_now_recv_info_t* info,const uint8_t* data,int n) {
  if(!info || !validFrame(data,n))return;
  RadioPacket p={};memcpy(&p.trama,data,n);memcpy(p.sender,info->src_addr,6);
  // El callback WiFi solo copia a una cola sincronizada; no hace NVS/HTTP/esperas.
  xQueueSend(meshRx,&p,0);
}
inline bool macSet(const uint8_t mac[6]) {for(int i=0;i<6;++i)if(mac[i])return true;return false;}
inline bool addPeer(const uint8_t mac[6]) {
  if(!macSet(mac))return false;if(esp_now_is_peer_exist(mac))return true;
  esp_now_peer_info_t p={};memcpy(p.peer_addr,mac,6);p.channel=0;p.ifidx=WIFI_IF_STA;
  return esp_now_add_peer(&p)==ESP_OK;
}
inline bool radioInit(uint8_t channel,bool hasWifi=false) {
  if(!hasWifi)WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);esp_wifi_get_mac(WIFI_IF_STA,ownMac);
  if(!hasWifi && esp_wifi_set_channel(channel,WIFI_SECOND_CHAN_NONE)!=ESP_OK)return false;
  meshRx=xQueueCreate(16,sizeof(RadioPacket));
  return meshRx && esp_now_init()==ESP_OK && esp_now_register_recv_cb(meshReceive)==ESP_OK;
}
inline bool radioSend(const uint8_t peer[6],const TramaMesh& t) {
  return addPeer(peer) && esp_now_send(peer,reinterpret_cast<const uint8_t*>(&t),frameBytes(t))==ESP_OK;
}
inline bool radioRead(RadioPacket& p) {return xQueueReceive(meshRx,&p,0)==pdTRUE;}
#endif
