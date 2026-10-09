#pragma once
#include "mesh_core.h"
#include "../../comun/protocolo_aura.h"
#pragma pack(push,1)
struct EnvioProtegido {
  uint32_t magic;
  uint8_t activo;
  MuestraAura muestra;
  uint8_t segundoId[16];
  uint32_t control;
};
#pragma pack(pop)

inline bool confirmarEnvio(Journal& j, const EnvioProtegido& out, const uint8_t id[16]) {
  if (!out.activo || !sameId(id,out.muestra.ingest_id)) return false;
  acknowledge(j,out.muestra.ingest_id);
  acknowledge(j,out.segundoId);
  return true;
}

inline bool journalContains(const Journal& j,const uint8_t id[16]) {
  for(int s=0;s<2;++s) {
    if(j.hasFirst[s]&&!j.firstAcked[s]&&j.first[s].kind==MEDICION&&sameId(j.first[s].id,id)) return true;
    for(int n=0;n<j.count[s];++n) {
      const auto& m=j.fifo[s][(j.head[s]+n)%MESH_FIFO_CAP];
      if(m.kind==MEDICION&&sameId(m.id,id)) return true;
    }
  }
  return false;
}
inline uint32_t pendingMeasurements(const Journal& j,const EnvioProtegido& f) {
  uint32_t n=0;
  for(int s=0;s<2;++s) {
    if(j.hasFirst[s]&&!j.firstAcked[s]&&j.first[s].kind==MEDICION) ++n;
    for(int i=0;i<j.count[s];++i) if(j.fifo[s][(j.head[s]+i)%MESH_FIFO_CAP].kind==MEDICION) ++n;
  }
  if(f.activo) {
    if(!journalContains(j,f.muestra.ingest_id)) ++n;
    bool hasSecond=false;for(int i=0;i<16;++i) hasSecond|=f.segundoId[i]!=0;
    if(hasSecond&&!journalContains(j,f.segundoId)) ++n;
  }
  return n;
}
