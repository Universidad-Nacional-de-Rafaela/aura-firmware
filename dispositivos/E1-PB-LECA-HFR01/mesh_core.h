#ifndef ICIV_MESH_CORE_H
#define ICIV_MESH_CORE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Extension IC_IV del encadenamiento ESP-NOW de aura-firmware. Los tres roles
// deben usar esta version; no confundir sus ACK finales con el ACK por salto v1.
const uint8_t MESH_VERSION = 3;
const size_t MESH_FIFO_CAP = 30;
const int16_t TEMP_INVALIDA = 32767;
enum MeshTipo : uint8_t { TELEMETRIA=1, COMANDO=2, ACK_SALTO=3,
  ACK_GATEWAY=4, ACK_CENTRAL=5, PROBE=6, PROBE_ACK=7, CONFIG_RESULT=8, CONFIG_SNAPSHOT=9, ACK_ALERT=10 };
enum RecordKind : uint8_t { MEDICION=0, ALERTA_SENSOR=1, ALERTA_ENERGIA=2 };
enum SensorState : uint8_t { SONDA_OK=0, SIN_RESPUESTA=1, FUERA_RANGO=2 };
enum SampleFlags : uint8_t { VALID=1, BATTERY=2, POWER_FIRST=4, CLOCK_VALID=8, POWER_KNOWN=16 };
#pragma pack(push, 1)
struct Muestra {
  uint8_t id[16];
  uint32_t measuredAt;
  uint32_t cutAt;
  int16_t temp100;
  uint8_t sensor;
  uint8_t flags;
  uint8_t kind, state;
};
struct SensorConfig { uint32_t intervalS; int16_t min100; int16_t max100; };
struct MeshConfig { SensorConfig sensors[2]; uint32_t recoveryS; };
struct ConfigCommand { char id[65]; uint16_t mask; MeshConfig config; };
struct ConfigResult { ConfigCommand command; uint8_t applied; };
struct NodeSnapshot {
  MeshConfig config;
  uint8_t pending[2];
  uint32_t dropped[2];
  uint8_t powerKnown,onBattery,sensorKnown[2],sensorValid[2];
  uint32_t cutAt;
  uint8_t sensorState[2];
};
struct TramaMesh {
  uint8_t version, tipo;
  uint8_t origen[6], destino[6];
  uint16_t seq;
  uint8_t largo;
  uint8_t payload[180];
};
struct Journal {
  uint32_t generation;
  MeshConfig config;
  Muestra fifo[2][MESH_FIFO_CAP];
  Muestra first[2];
  uint8_t head[2], count[2], hasFirst[2], firstAcked[2];
  uint32_t dropped[2], missedCuts;
  uint8_t powerKnown, onBattery, capturePending;
  uint32_t cutAt;
  uint8_t lastConfigId[16];
  uint8_t sensorKnown[2],sensorState[2];
  Muestra firstPower;
  uint8_t hasPower,powerAcked;
};
#pragma pack(pop)
static_assert(sizeof(Muestra)==30, "Formato de muestra v3");
static_assert(sizeof(ConfigResult)<=180, "Configuracion dentro del payload ESP-NOW");
static_assert(offsetof(TramaMesh,payload)==17, "Cabecera AURA de 17 bytes");
static_assert(sizeof(Journal)<2048, "Journal compacto: menos de 2 KB");
inline constexpr MeshConfig meshDefaults() { return {{{60,200,600},{300,-2500,-1500}},300}; }
inline constexpr bool validSensorConfig(const SensorConfig& c) {
  return c.intervalS>=5 && c.intervalS<=86400 && c.min100>=-5500 &&
    c.max100<=12500 && c.min100<c.max100;
}
inline constexpr bool validConfig(const MeshConfig& c) {
  return validSensorConfig(c.sensors[0]) && validSensorConfig(c.sensors[1]) &&
    c.recoveryS>=60 && c.recoveryS<=86400;
}
inline constexpr bool sameId(const uint8_t a[16],const uint8_t b[16]) {for(int i=0;i<16;++i)if(a[i]!=b[i])return false;return true;}
inline constexpr bool validSample(const Muestra& s) {
  return s.sensor<2 && s.kind<=ALERTA_ENERGIA && s.state<=2 && !(s.flags & ~31) && (s.kind==MEDICION
    ? (s.flags&VALID) && s.temp100>=-5500 && s.temp100<=12500 && s.temp100!=8500
    : !(s.flags&VALID) && s.temp100==TEMP_INVALIDA) &&
    ((s.flags&CLOCK_VALID) ? s.measuredAt>=1700000000UL : s.measuredAt==0);
}
inline constexpr void journalInit(Journal& j) {j=Journal{};j.config=meshDefaults();}
inline constexpr bool validJournal(const Journal& j) {
  if (!validConfig(j.config) || j.powerKnown>1 || j.onBattery>1 || j.capturePending>1) return false;
  if(j.hasPower>1||j.powerAcked>1||(j.hasPower && (!validSample(j.firstPower)||j.firstPower.kind!=ALERTA_ENERGIA)))return false;
  for (int s=0;s<2;++s) {
    if(j.sensorKnown[s]>1 || j.sensorState[s]>2)return false;
    if (j.head[s]>=MESH_FIFO_CAP || j.count[s]>MESH_FIFO_CAP || j.hasFirst[s]>1 || j.firstAcked[s]>1) return false;
    if (j.hasFirst[s] && (!validSample(j.first[s]) || j.first[s].sensor!=s)) return false;
    for (int i=0;i<j.count[s];++i) if (!validSample(j.fifo[s][(j.head[s]+i)%MESH_FIFO_CAP]) || j.fifo[s][(j.head[s]+i)%MESH_FIFO_CAP].sensor!=s) return false;
  }
  return true;
}
inline constexpr void pushSample(Journal& j,const Muestra& m) {
  const int s=m.sensor;
  if (j.count[s]==MESH_FIFO_CAP) { j.head[s]=(j.head[s]+1)%MESH_FIFO_CAP; --j.count[s]; ++j.dropped[s]; }
  j.fifo[s][(j.head[s]+j.count[s])%MESH_FIFO_CAP]=m; ++j.count[s];
}
inline constexpr bool protectFirst(Journal& j,const Muestra& m) {
  const int s=m.sensor;
  if (j.hasFirst[s] && !j.firstAcked[s]) { ++j.missedCuts; return false; }
  j.first[s]=m; j.hasFirst[s]=1; j.firstAcked[s]=0; return true;
}
inline constexpr bool nextSample(const Journal& j,Muestra& out) {
  bool found=j.hasPower&&!j.powerAcked;if(found)out=j.firstPower;
  for(int s=0;s<2;++s) {
    const Muestra* m=nullptr;
    if(j.hasFirst[s] && !j.firstAcked[s]) m=&j.first[s];
    else if(j.count[s]) m=&j.fifo[s][j.head[s]];
    if(m && (!found || (m->measuredAt && (!out.measuredAt || m->measuredAt<out.measuredAt)))) {out=*m;found=true;}
  }
  return found;
}
inline constexpr bool acknowledge(Journal& j,const uint8_t id[16]) {
  if(j.hasPower && sameId(j.firstPower.id,id)){j.powerAcked=1;return true;}
  for(int s=0;s<2;++s) {
    if(j.hasFirst[s] && sameId(j.first[s].id,id)) {j.firstAcked[s]=1;return true;}
    if(j.count[s] && sameId(j.fifo[s][j.head[s]].id,id)) {j.head[s]=(j.head[s]+1)%MESH_FIFO_CAP;--j.count[s];return true;}
  }
  return false; // un ACK atrasado no elimina otra muestra
}
inline constexpr uint32_t crc32(const uint8_t* p,size_t n) {
  uint32_t crc=0xFFFFFFFF;
  for(size_t i=0;i<n;++i) {crc^=p[i];for(int b=0;b<8;++b)crc=(crc>>1)^((crc&1)?0xEDB88320:0);}
  return ~crc;
}
inline constexpr bool centralAckAllowed(int http,int inserted,int duplicates,int errors,int sent=1) {
  return http==201 && sent==1 && inserted>=0 && duplicates>=0 && errors==0 && inserted<=1 && duplicates<=1 && inserted+duplicates==sent;
}
inline constexpr bool applyPatch(const MeshConfig& current,const ConfigCommand& cmd,MeshConfig& next) {
  if(!cmd.mask || (cmd.mask&~127))return false;
  next=current;
  if(cmd.mask&1)next.sensors[0].intervalS=cmd.config.sensors[0].intervalS;
  if(cmd.mask&2)next.sensors[1].intervalS=cmd.config.sensors[1].intervalS;
  if(cmd.mask&4)next.sensors[0].min100=cmd.config.sensors[0].min100;
  if(cmd.mask&8)next.sensors[0].max100=cmd.config.sensors[0].max100;
  if(cmd.mask&16)next.sensors[1].min100=cmd.config.sensors[1].min100;
  if(cmd.mask&32)next.sensors[1].max100=cmd.config.sensors[1].max100;
  if(cmd.mask&64)next.recoveryS=cmd.config.recoveryS;
  return validConfig(next);
}
inline constexpr uint8_t classifyTemperature(float t,bool conversionReady) {
  return !conversionReady || t!=t || t==-127?SIN_RESPUESTA:(t<-55||t>125||t==85)?FUERA_RANGO:SONDA_OK;
}
inline size_t frameBytes(const TramaMesh& t) {return 17+t.largo;}
inline bool validFrame(const uint8_t* p,int n) {
  if(!p || n<17) return false;
  const TramaMesh* t=reinterpret_cast<const TramaMesh*>(p);
  return t->version==MESH_VERSION && t->tipo>=1 && t->tipo<=10 && t->largo<=180 && n==17+t->largo;
}
inline TramaMesh frame(uint8_t type,const uint8_t origin[6],const uint8_t dest[6],uint16_t seq,const void* payload,size_t size) {
  TramaMesh t={};t.version=MESH_VERSION;t.tipo=type;memcpy(t.origen,origin,6);memcpy(t.destino,dest,6);t.seq=seq;
  if(size<=180){t.largo=size;if(payload && size)memcpy(t.payload,payload,size);}return t;
}
#endif
