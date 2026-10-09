#include "../adaptador_aura.h"
#include <cassert>
#include <iostream>
Muestra muestra(int s,int id,uint32_t t) { Muestra m={};m.sensor=s;m.id[0]=id;m.measuredAt=t;m.temp100=s?-1800:400;m.flags=VALID|(t?CLOCK_VALID:0);return m; }
int main() {
  const char* error;MeshConfig next,current=meshDefaults();JsonDocument d;
  auto patch=[&](const char* json) { assert(!deserializeJson(d,json));bool ok=leerConfig(d.as<JsonObjectConst>(),current,next,error); if(!ok) std::cerr<<error<<": "<<json<<std::endl;return ok; };
  assert(patch("{\"intervalo_s\":120,\"f_s\":600}"));assert(next.sensors[0].intervalS==120&&next.sensors[1].intervalS==600);
  for(const char* invalid:{"{}","{\"intervalo_s\":true}","{\"intervalo_s\":5.0}","{\"f_s\":4}","{\"f_s\":86401}","{\"f_s\":-1}","{\"r_s\":59}","{\"hlo\":7}","{\"hlo\":\"2\"}","{\"hlo\":true}","{\"hlo\":2.001}","{\"otra\":1}","{\"intervalo_s\":5,\"fhi\":-30}"}) assert(!patch(invalid));
  assert(current.sensors[0].intervalS==60);assert(patch("{\"hlo\":-54.99,\"hhi\":124.99,\"flo\":-54.99,\"fhi\":124.99,\"intervalo_s\":86400,\"f_s\":86400,\"r_s\":86400}"));
  JsonDocument report;describirConfig(next,report["config"].to<JsonObject>());
  report["pendientes"]=1502;report["descartadas"]=UINT32_MAX;report["alimentacion"]="desconocida";
  std::cout<<"Reporte maximo="<<measureJson(report)<<" bytes\n";assert(measureJson(report)<=AURA_PAYLOAD_MAX);
  JsonDocument result;result["command_id"]="00000000-0000-4000-8000-000000000000";result["aplicado"]=true;
  describirConfig(next,result["config"].to<JsonObject>());assert(measureJson(result)<=AURA_PAYLOAD_MAX);
  JsonDocument sensor;sensor["tipo"]="sensor";sensor["severity"]="high";sensor["message"]="Sonda temp_heladera_c con falla";
  sensor["details"]["campo"]="temp_heladera_c";sensor["details"]["motivo"]="fuera_de_rango";assert(measureJson(sensor)<=AURA_PAYLOAD_MAX);
  Journal j; journalInit(j);pushSample(j,muestra(0,1,1800000000));pushSample(j,muestra(1,2,1800000000));
  EnvioProtegido f;assert(prepararEnvio(j,f));char json[AURA_VALUES_MAX+1]={};memcpy(json,f.muestra.values,f.muestra.largo);
  JsonDocument values;assert(!deserializeJson(values,json));assert(values.size()==2&&values["temp_heladera_c"]==4&&values["temp_freezer_c"]==-18);
  assert(f.segundoId[0]==2&&f.muestra.ts==1800000000);
  TramaAura frame;assert(aura_telemetria_armar(&frame,7,&f.muestra));MuestraAura decoded;assert(aura_telemetria_leer(&frame,&decoded));assert(!memcmp(&decoded,&f.muestra,sizeof(decoded)));
  journalInit(j);pushSample(j,muestra(0,3,0));pushSample(j,muestra(1,4,0));assert(prepararEnvio(j,f));assert(f.segundoId[0]==0&&f.muestra.ts==0);
  journalInit(j);auto old=muestra(0,5,1800000000);protectFirst(j,old);pushSample(j,muestra(1,6,1800000001));assert(prepararEnvio(j,f));assert(f.segundoId[0]==0);
  for(int n=7;n<90;++n)pushSample(j,muestra(0,n,1800000100+n));assert(!j.firstAcked[0]&&sameId(j.first[0].id,old.id));assert(prepararEnvio(j,f)&&f.muestra.ingest_id[0]==5);
  std::cout<<"JSON v4: tipos/rangos/atomicidad, 180 B, ambas sondas, hora y primera captura OK\n";
}
