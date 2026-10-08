#pragma once
// Adaptacion propia, comprobable sin placa: JSON, configuracion y agrupacion.
#include <ArduinoJson.h>
#include <cmath>
#include "modelo_envio.h"
#include "protocolo_aura.h"

inline void describirConfig(const MeshConfig& c, JsonObject out) {
  // Nombres compactos: reporte completo <= 180 bytes incluso en los extremos.
  out["intervalo_s"] = c.sensors[0].intervalS;
  out["f_s"] = c.sensors[1].intervalS;
  out["hlo"] = double(c.sensors[0].min100) / 100;
  out["hhi"] = double(c.sensors[0].max100) / 100;
  out["flo"] = double(c.sensors[1].min100) / 100;
  out["fhi"] = double(c.sensors[1].max100) / 100;
  out["r_s"] = c.recoveryS;
}

inline bool leerConfig(JsonObjectConst params, const MeshConfig& current,
                       MeshConfig& next, const char*& motivo) {
  if (params.isNull() || !params.size()) { motivo="sin_parametros"; return false; }
  next=current;
  for (JsonPairConst p : params) {
    const char* k=p.key().c_str(); JsonVariantConst v=p.value();
    if (!strcmp(k,"intervalo_s") || !strcmp(k,"f_s") || !strcmp(k,"r_s")) {
      if (!v.is<uint32_t>()) { motivo="intervalo_no_entero"; return false; }
      if (!strcmp(k,"intervalo_s")) next.sensors[0].intervalS=v.as<uint32_t>();
      else if (!strcmp(k,"f_s")) next.sensors[1].intervalS=v.as<uint32_t>();
      else next.recoveryS=v.as<uint32_t>();
    } else if (!strcmp(k,"hlo") || !strcmp(k,"hhi") || !strcmp(k,"flo") || !strcmp(k,"fhi")) {
      if (v.is<bool>() || !v.is<double>()) { motivo="umbral_no_numerico"; return false; }
      double x=v.as<double>(), scaled=x*100;
      if (!std::isfinite(x) || x < -55 || x > 125 || std::fabs(scaled-std::round(scaled)) > 0.001) {
        motivo="umbral_fuera_de_rango"; return false;
      }
      auto& s=next.sensors[k[0]=='f'?1:0];
      if (k[1]=='l') s.min100=int16_t(std::lround(scaled)); else s.max100=int16_t(std::lround(scaled));
    } else { motivo="parametro_desconocido"; return false; }
  }
  if (!validConfig(next)) { motivo="config_fuera_de_rango"; return false; }
  motivo=""; return true;
}

inline bool sensorHead(const Journal& j, int s, Muestra& out) {
  if (j.hasFirst[s] && !j.firstAcked[s]) { out=j.first[s]; return true; }
  if (j.count[s]) { out=j.fifo[s][j.head[s]]; return true; }
  return false;
}

inline bool prepararEnvio(const Journal& j, EnvioProtegido& out) {
  Muestra first;
  if (!nextSample(j,first) || first.kind!=MEDICION) return false;
  out={}; out.magic=0x49434134; out.activo=1;
  memcpy(out.muestra.ingest_id,first.id,16);
  out.muestra.ts=first.measuredAt && aura_hora_valida(first.measuredAt)?first.measuredAt:0;
  JsonDocument values;
  const char* campos[2]={"temp_heladera_c","temp_freezer_c"};
  values[campos[first.sensor]]=double(first.temp100)/100;
  Muestra second;
  // Sin hora no se supone que dos registros se midieron juntos.
  if (first.measuredAt && sensorHead(j,1-first.sensor,second) && second.kind==MEDICION &&
      second.measuredAt==first.measuredAt && (second.flags&POWER_FIRST)==(first.flags&POWER_FIRST)) {
    values[campos[second.sensor]]=double(second.temp100)/100;
    memcpy(out.segundoId,second.id,16);
  }
  size_t n=measureJson(values);
  if (n>AURA_VALUES_MAX) return false;
  char json[AURA_VALUES_MAX+1]; serializeJson(values,json,sizeof(json));
  out.muestra.largo=uint8_t(n); memcpy(out.muestra.values,json,n);
  out.control=crc32(reinterpret_cast<const uint8_t*>(&out),offsetof(EnvioProtegido,control));
  return true;
}
