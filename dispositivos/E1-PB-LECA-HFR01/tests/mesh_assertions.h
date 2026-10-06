#ifndef ICIV_MESH_ASSERTIONS_H
#define ICIV_MESH_ASSERTIONS_H
#include "../mesh_core.h"
constexpr Muestra sample(int s,int n) {
  Muestra m={};m.sensor=s;m.id[0]=s+1;m.id[1]=n;m.measuredAt=1800000000UL+n;m.temp100=s?-1800:400;m.flags=VALID|CLOCK_VALID;return m;
}
constexpr bool fifoAndFirst() {
  Journal j={};journalInit(j);const auto first=sample(0,0);
  if(!protectFirst(j,first))return false;
  for(int i=1;i<=70;++i)pushSample(j,sample(0,i));
  if(j.count[0]!=30 || j.dropped[0]!=40 || !sameId(j.first[0].id,first.id))return false;
  Muestra m={};if(!nextSample(j,m)||!sameId(m.id,first.id))return false;
  if(!acknowledge(j,first.id) || !j.hasFirst[0] || !j.firstAcked[0])return false;
  if(!nextSample(j,m) || m.id[1]!=41)return false;
  for(int i=41;i<=70;++i){if(!acknowledge(j,m.id))return false;if(i<70 && !nextSample(j,m))return false;}
  return j.count[0]==0 && !nextSample(j,m) && validJournal(j);
}
constexpr bool twoSensorsAndRestart() {
  Journal j={};journalInit(j);protectFirst(j,sample(0,1));protectFirst(j,sample(1,1));
  pushSample(j,sample(0,2));pushSample(j,sample(1,2));
  Journal restored=j;Muestra out={};nextSample(restored,out);
  uint8_t wrong[16]={99};if(acknowledge(restored,wrong))return false;
  if(!acknowledge(restored,sample(0,1).id)||!acknowledge(restored,sample(1,1).id))return false;
  if(restored.count[0]!=1 || restored.count[1]!=1)return false;
  return sameId(restored.first[0].id,j.first[0].id)&&validJournal(restored);
}
constexpr bool repeatedCut() {
  Journal j={};journalInit(j);protectFirst(j,sample(0,1));
  if(protectFirst(j,sample(0,2))||j.missedCuts!=1)return false;
  if(!sameId(j.first[0].id,sample(0,1).id))return false;
  acknowledge(j,sample(0,1).id);return protectFirst(j,sample(0,3))&&sameId(j.first[0].id,sample(0,3).id);
}
constexpr uint8_t crcCheck[]={49,50,51,52,53,54,55,56,57};
static_assert(fifoAndFirst(),"FIFO: conserva primera, pierde solo antiguas ordinarias y respeta orden");
static_assert(twoSensorsAndRestart(),"Sondas y copia restaurada: ACK ajeno no borra datos");
static_assert(repeatedCut(),"Segundo corte no pisa primero pendiente");
static_assert(crc32(crcCheck,9)==0xCBF43926,"CRC32 de referencia");
static_assert(!validSensorConfig({4,200,600}),"Rechazar muestreo demasiado corto");
static_assert(!validSensorConfig({86401,200,600}),"Rechazar intervalo demasiado largo");
static_assert(!validSensorConfig({60,600,200}),"Rechazar rango invertido");
static_assert(validConfig(meshDefaults()),"Defaults de ambas sondas validos");
static_assert(centralAckAllowed(201,1,0,0),"REST inserto exactamente una muestra");
static_assert(centralAckAllowed(201,0,1,0),"Duplicado es persistido");
static_assert(!centralAckAllowed(201,0,0,1),"201 con errors=1 no elimina muestra");
static_assert(!centralAckAllowed(201,1,0,1),"Error parcial no confirma");
static_assert(!centralAckAllowed(200,1,0,0),"Contrato exige HTTP 201");
static_assert(!centralAckAllowed(201,0,0,0),"Sin insertar ni duplicado no confirma");
static_assert(!centralAckAllowed(201,-1,2,0),"Contadores invalidos no confirman");
static_assert(!centralAckAllowed(201,1,1,0),"Cuenta incorrecta no confirma");
static_assert(classifyTemperature(85,true)==FUERA_RANGO,"85 de arranque no es medicion");
static_assert(classifyTemperature(4,false)==SIN_RESPUESTA,"Exigir conversion completa");
static_assert(classifyTemperature(-127,true)==SIN_RESPUESTA,"Centinela ausente");
static_assert(classifyTemperature(-18,true)==SONDA_OK,"Freezer real valido");
constexpr bool atomicConfig(){ConfigCommand c={};c.mask=4;c.config.sensors[0].min100=700;MeshConfig next={};return !applyPatch(meshDefaults(),c,next);}
constexpr bool partialConfig(){ConfigCommand c={};c.mask=1;c.config.sensors[0].intervalS=10;MeshConfig next={};return applyPatch(meshDefaults(),c,next)&&next.sensors[1].intervalS==300&&next.recoveryS==300;}
static_assert(atomicConfig(),"Parche invalido no se aplica");
static_assert(partialConfig(),"Parche conserva parametros no enviados");
#endif
