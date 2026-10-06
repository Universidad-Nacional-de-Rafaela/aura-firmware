// conectividad con el gateway

#ifndef ICIV_NODO_COMUNICACION_H
#define ICIV_NODO_COMUNICACION_H

// Entrega ESP-NOW, ACK, reintentos, recuperacion y configuracion remota.
#include "almacenamiento.h"
#include "pantalla.h"

void beginNetwork() {
  radioOk=radioInit(ICIV_CHANNEL);radioRunning=radioOk;
  if(radioOk)addPeer(parentMac);
}
void checkNetworkConfiguration() {
  if(!radioOk || !macSet(parentMac)||!macSet(gatewayMac))linkState=GATEWAY_DOWN;
}

void wakeRadio() {
  if(!radioRunning && radioOk) {radioRunning=esp_wifi_start()==ESP_OK;}
}

void failDelivery(uint32_t now) {
  linkState=everGateway?CENTRAL_DOWN:GATEWAY_DOWN;waiting=false;scanning=false;
  forceSnapshot=false;
  nextRecovery=now+journal.config.recoveryS*1000UL;
  // Offline limita transmisiones, no apaga la escucha de comandos (contrato
  // AURA 3.3). Si el barrido fallo, volver al ultimo canal conocido del padre.
  if(radioRunning)esp_wifi_set_channel(listeningChannel,WIFI_SECOND_CHAN_NONE);
  updateOutputs();
  Serial.println(everGateway?"[OFFLINE] Gateway confirmo; falta central":"[OFFLINE] Sin confirmacion del gateway final");
}

void transmit(uint32_t now) {
  gatewayReceived=false;sentAt=now;++attempts;
  TramaMesh t=frame(TELEMETRIA,ownMac,gatewayMac,seq++,&flight,sizeof(flight));
  radioSend(parentMac,t); // Sin ACK final no se elimina de NVS.
  lastRadioSend=now;
}

void receiveMessages(uint32_t now) {
  RadioPacket p;
  while(radioRead(p)) {
    const auto& t=p.trama;
    if(memcmp(p.sender,parentMac,6) || memcmp(t.origen,gatewayMac,6) || memcmp(t.destino,ownMac,6))continue;
    if(t.tipo==PROBE_ACK && scanning && t.largo==0) {
      wifi_second_chan_t second;esp_wifi_get_channel(&listeningChannel,&second);
      forceSnapshot=true;
      scanning=false;waiting=true;attempts=0;everGateway=false;transmit(now);continue;
    }
    if(waiting && t.largo==16 && sameId(t.payload,flight.id)) {
      if(t.tipo==ACK_GATEWAY) {gatewayReceived=true;everGateway=true;}
      if((t.tipo==ACK_CENTRAL && flight.kind==MEDICION)||(t.tipo==ACK_ALERT && flight.kind!=MEDICION)) {
        acknowledge(journal,flight.id);
        if(!commit())return;
        waiting=false;if(t.tipo==ACK_CENTRAL)linkState=ONLINE;attempts=0;nextRecovery=now;snapshotPending=true;updateOutputs();
        Serial.println(t.tipo==ACK_CENTRAL?"[ACK] AURA REST persistio muestra":"[ACK ALERTA] Gateway publico en MQTT; no acredita persistencia central");
      }
    }
    if(t.tipo==COMANDO && t.largo==sizeof(ConfigCommand)) {
      ConfigCommand cmd;memcpy(&cmd,t.payload,sizeof(cmd));
      ConfigResult result={cmd,0};MeshConfig next;
      if(memchr(cmd.id,0,sizeof(cmd.id)) && applyPatch(journal.config,cmd,next) && storageOk) {
        MeshConfig old=journal.config;journal.config=next;
        if(commit())result.applied=1;else journal.config=old;
      }
      result.command.config=journal.config;
      if(result.applied)updateOutputs();
      radioSend(parentMac,frame(CONFIG_RESULT,ownMac,gatewayMac,t.seq,&result,sizeof(result)));
      lastRadioSend=now;snapshotPending=true;forceSnapshot=true;
    }
  }
}

void sendSnapshot(uint32_t now) {
  if(!snapshotPending || !storageOk || !radioRunning || scanning || uint32_t(now-lastRadioSend)<30)return;
  if(!macSet(parentMac)||!macSet(gatewayMac))return;
  if((linkState==GATEWAY_DOWN||linkState==CENTRAL_DOWN)&&!forceSnapshot)return;
  NodeSnapshot snapshot={};snapshot.config=journal.config;
  snapshot.powerKnown=ICIV_POWER_PIN>=0 && journal.powerKnown;snapshot.onBattery=battery;snapshot.cutAt=journal.cutAt;
  for(int s=0;s<2;++s) {
    snapshot.pending[s]=journal.count[s]+(journal.hasFirst[s]&&!journal.firstAcked[s]?1:0);
    snapshot.dropped[s]=journal.dropped[s];snapshot.sensorKnown[s]=sensorKnown[s];snapshot.sensorValid[s]=journal.sensorState[s]==SONDA_OK;snapshot.sensorState[s]=journal.sensorState[s];
  }
  snapshot.pending[0]+=journal.hasPower&&!journal.powerAcked?1:0;
  if(radioSend(parentMac,frame(CONFIG_SNAPSHOT,ownMac,gatewayMac,seq++,&snapshot,sizeof(snapshot)))){snapshotPending=false;forceSnapshot=false;}
  lastRadioSend=now;
}

void startScan(uint32_t now) {
  wakeRadio();if(!radioRunning){failDelivery(now);return;}
  scanning=true;scanChannel=1;scanAt=now-500;
}

void serviceNetwork(uint32_t now) {
  if(!storageOk || !radioOk || !macSet(parentMac) || !macSet(gatewayMac))return;
  if(radioRunning)receiveMessages(now);
  if(scanning) {
    if(uint32_t(now-scanAt)>=500) {
      if(scanChannel>11){failDelivery(now);return;}
      esp_wifi_set_channel(scanChannel++,WIFI_SECOND_CHAN_NONE);scanAt=now;
      radioSend(parentMac,frame(PROBE,ownMac,gatewayMac,seq++,nullptr,0));
    }
    return;
  }
  if(waiting) {
    const uint32_t timeout=gatewayReceived?12000:4000;
    if(uint32_t(now-sentAt)>=timeout) {
      const uint8_t budget=(linkState==GATEWAY_DOWN||linkState==CENTRAL_DOWN)?1:4;
      if(attempts>=budget)failDelivery(now);else transmit(now);
    }
    return;
  }
  if(!nextSample(journal,flight))return;
  if(linkState==GATEWAY_DOWN||linkState==CENTRAL_DOWN) {
    if(int32_t(now-nextRecovery)<0)return;
    startScan(now);return;
  }
  waiting=true;everGateway=false;attempts=0;transmit(now);
}

#endif
