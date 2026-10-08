#ifndef ICIV_MESH_STORAGE_H
#define ICIV_MESH_STORAGE_H
#include <Preferences.h>
#include "mesh_core.h"
// Dos bancos por registro: escribir banco inactivo y despues confirmar indice.
// Un corte antes del indice no modifica la cola que sigue referenciada.
#pragma pack(push,1)
struct QueueIndex {
  uint32_t magic,generation;
  MeshConfig config;
  uint8_t head[2],count[2],hasFirst[2],firstAcked[2];
  uint32_t dropped[2],missedCuts,cutAt;
  uint8_t powerKnown,onBattery,capturePending,lastConfigId[16];
  uint8_t sensorKnown[2],sensorState[2];
  uint8_t hasPower,powerAcked;
  uint64_t banks;
  uint32_t recordCrc[63];
  uint32_t crc;
};
struct StoredRecord {Muestra sample;uint32_t crc;};
#pragma pack(pop)
static_assert(sizeof(QueueIndex)<360,"Indice menor que el journal");
class MeshStorage {
  Preferences prefs;
  QueueIndex active={},candidate={};
  Journal previous={};
  uint8_t slot=0;
  bool activeSlot(const Journal& j,int s,int pos) {
    for(int i=0;i<j.count[s];++i)if((j.head[s]+i)%30==pos)return true;
    return false;
  }
  void key(char out[12],int record,bool bank){snprintf(out,12,"r%02d%c",record,bank?'b':'a');}
  bool validIndex(QueueIndex& index) {
    return index.magic==0x49433333 && index.crc==crc32(reinterpret_cast<uint8_t*>(&index),offsetof(QueueIndex,crc));
  }
  bool load(int n,QueueIndex& index,Journal& j) {
    const char* name=n?"index1":"index0";
    if(prefs.getBytesLength(name)!=sizeof(index)||prefs.getBytes(name,&index,sizeof(index))!=sizeof(index)||!validIndex(index))return false;
    journalInit(j);j.generation=index.generation;j.config=index.config;
    memcpy(j.head,index.head,2);memcpy(j.count,index.count,2);memcpy(j.hasFirst,index.hasFirst,2);memcpy(j.firstAcked,index.firstAcked,2);
    memcpy(j.dropped,index.dropped,8);j.missedCuts=index.missedCuts;j.cutAt=index.cutAt;
    j.powerKnown=index.powerKnown;j.onBattery=index.onBattery;j.capturePending=index.capturePending;memcpy(j.lastConfigId,index.lastConfigId,16);memcpy(j.sensorKnown,index.sensorKnown,2);memcpy(j.sensorState,index.sensorState,2);
    if(!validConfig(j.config)||j.count[0]>30||j.count[1]>30||j.head[0]>=30||j.head[1]>=30)return false;
    for(int s=0;s<2;++s)for(int i=0;i<31;++i) {
      const bool first=i==30;if(first?!j.hasFirst[s]:!activeSlot(j,s,i))continue;
      const int r=s*31+i;char name[12];key(name,r,(index.banks>>r)&1);StoredRecord b={};
      if(prefs.getBytesLength(name)!=sizeof(b)||prefs.getBytes(name,&b,sizeof(b))!=sizeof(b))return false;
      if(b.crc!=index.recordCrc[r]||b.crc!=crc32(reinterpret_cast<uint8_t*>(&b.sample),sizeof(Muestra))||!validSample(b.sample)||b.sample.sensor!=s)return false;
      if(first)j.first[s]=b.sample;else j.fifo[s][i]=b.sample;
    }
    j.hasPower=index.hasPower;j.powerAcked=index.powerAcked;
    if(j.hasPower) {
      char name[12];key(name,62,(index.banks>>62)&1);StoredRecord b={};
      if(prefs.getBytesLength(name)!=sizeof(b)||prefs.getBytes(name,&b,sizeof(b))!=sizeof(b)||b.crc!=index.recordCrc[62]||b.crc!=crc32(reinterpret_cast<uint8_t*>(&b.sample),sizeof(Muestra)))return false;
      j.firstPower=b.sample;
    }
    return validJournal(j);
  }
public:
  bool begin(Journal& j) {
    if(!prefs.begin("iciv_mesh3",false))return false;
    bool a=load(0,active,j),b=load(1,candidate,previous);
    if(a||b) {
      slot=b&&(!a||int32_t(candidate.generation-active.generation)>0);
      if(slot){active=candidate;j=previous;}previous=j;return true;
    }
    if(prefs.isKey("index0")||prefs.isKey("index1"))return false;
    // No borrar la cola anterior al cambiar de formato.
    Preferences legacy;
    if(legacy.begin("iciv_mesh",true)) {
      bool exists=legacy.isKey("q0")||legacy.isKey("q1");legacy.end();
      if(exists){Serial.println("[MIGRACION] NVS v2 presente: exportar/vaciar y revisar antes de v3; no se borro");return false;}
    }
    journalInit(j);previous=j;return save(j);
  }
  bool save(Journal& j) {
    if(!validJournal(j))return false;
    candidate=active;candidate.magic=0x49433333;candidate.generation=j.generation+1;candidate.config=j.config;
    memcpy(candidate.head,j.head,2);memcpy(candidate.count,j.count,2);memcpy(candidate.hasFirst,j.hasFirst,2);memcpy(candidate.firstAcked,j.firstAcked,2);
    memcpy(candidate.dropped,j.dropped,8);candidate.missedCuts=j.missedCuts;candidate.cutAt=j.cutAt;
    candidate.powerKnown=j.powerKnown;candidate.onBattery=j.onBattery;candidate.capturePending=j.capturePending;memcpy(candidate.lastConfigId,j.lastConfigId,16);memcpy(candidate.sensorKnown,j.sensorKnown,2);memcpy(candidate.sensorState,j.sensorState,2);
    for(int s=0;s<2;++s)for(int i=0;i<31;++i) {
      bool first=i==30;if(first?!j.hasFirst[s]:!activeSlot(j,s,i))continue;
      const Muestra& m=first?j.first[s]:j.fifo[s][i];const Muestra& old=first?previous.first[s]:previous.fifo[s][i];
      bool oldActive=first?previous.hasFirst[s]:activeSlot(previous,s,i);
      int r=s*31+i;
      if(oldActive && !memcmp(&m,&old,sizeof(m)))continue;
      bool bank=!((active.banks>>r)&1);char name[12];key(name,r,bank);
      StoredRecord record={m,crc32(reinterpret_cast<const uint8_t*>(&m),sizeof(m))};
      if(prefs.putBytes(name,&record,sizeof(record))!=sizeof(record))return false;
      StoredRecord check={};if(prefs.getBytes(name,&check,sizeof(check))!=sizeof(check)||memcmp(&record,&check,sizeof(record)))return false;
      candidate.banks=(candidate.banks&~(uint64_t(1)<<r))|(uint64_t(bank)<<r);candidate.recordCrc[r]=record.crc;
    }
    candidate.hasPower=j.hasPower;candidate.powerAcked=j.powerAcked;
    if(j.hasPower && (!previous.hasPower || memcmp(&j.firstPower,&previous.firstPower,sizeof(Muestra)))) {
      bool bank=!((active.banks>>62)&1);char name[12];key(name,62,bank);
      StoredRecord b={j.firstPower,crc32(reinterpret_cast<const uint8_t*>(&j.firstPower),sizeof(Muestra))};
      if(prefs.putBytes(name,&b,sizeof(b))!=sizeof(b))return false;
      StoredRecord check={};if(prefs.getBytes(name,&check,sizeof(check))!=sizeof(check)||memcmp(&b,&check,sizeof(b)))return false;
      candidate.banks=(candidate.banks&~(uint64_t(1)<<62))|(uint64_t(bank)<<62);candidate.recordCrc[62]=b.crc;
    }
    candidate.crc=crc32(reinterpret_cast<const uint8_t*>(&candidate),offsetof(QueueIndex,crc));
    const char* name=slot?"index0":"index1";
    if(prefs.putBytes(name,&candidate,sizeof(candidate))!=sizeof(candidate))return false;
    QueueIndex check={};if(prefs.getBytes(name,&check,sizeof(check))!=sizeof(check)||memcmp(&candidate,&check,sizeof(check)))return false;
    slot=1-slot;active=candidate;j.generation=candidate.generation;previous=j;return true;
  }
};
#endif
