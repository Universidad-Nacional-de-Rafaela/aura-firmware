#include "fakes/Preferences.h"
#include "../envio_persistente.h"
#include "../politica_envio.h"
#include <cassert>
#include <iostream>
Muestra muestra(int s,int n) { Muestra m={};m.sensor=s;m.id[0]=n;m.temp100=s?-1800:400;m.flags=VALID;return m; }
int main() {
  PoliticaEnvio policy;
  for(int n=0;n<4;++n) policy.intento(n*1000,true,300000);
  policy.revisar(122999,120000,300000);assert(!policy.lenta);
  policy.revisar(123000,120000,300000);assert(policy.lenta&&!policy.corresponde(422999)&&policy.corresponde(423000));
  policy.intento(423000,false,300000);assert(!policy.corresponde(423001)&&policy.proximo==723000);
  policy.confirmar();assert(!policy.lenta&&policy.intentos==0);
  for(int n=0;n<4;++n) policy.intento(n*2000,false,300000);
  assert(policy.lenta&&policy.proximo==306000);
  policy.confirmar();policy.ultimo=UINT32_MAX-100;policy.intentos=4;
  policy.revisar(50,151,300000);assert(policy.lenta);
  FakeNvs::reset();Journal j; journalInit(j);
  auto a=muestra(0,1),b=muestra(1,2); protectFirst(j,a); pushSample(j,b);
  EnvioProtegido f={};f.magic=0x49434134;f.activo=1;
  memcpy(f.muestra.ingest_id,a.id,16);memcpy(f.segundoId,b.id,16);
  const char* values="{\"temp_heladera_c\":4,\"temp_freezer_c\":-18}";
  f.muestra.largo=strlen(values);memcpy(f.muestra.values,values,f.muestra.largo);
  f.control=crc32(reinterpret_cast<uint8_t*>(&f),offsetof(EnvioProtegido,control));
  assert(pendingMeasurements(j,f)==2);
  EnvioPersistente store;EnvioProtegido loaded;assert(store.begin(loaded)&&!loaded.activo);assert(store.save(f));
  EnvioPersistente reboot;assert(reboot.begin(loaded));assert(!memcmp(&f,&loaded,sizeof(f)));
  uint8_t foreign[16]={99};assert(!confirmarEnvio(j,f,foreign));assert(!j.firstAcked[0]&&j.count[1]==1);
  assert(confirmarEnvio(j,f,a.id));assert(j.firstAcked[0]&&j.count[1]==0);
  // Corte entre el ACK del journal y la limpieza: el mensaje conservado es identico.
  EnvioPersistente afterAck;assert(afterAck.begin(loaded));assert(!memcmp(&f,&loaded,sizeof(f)));
  assert(pendingMeasurements(j,loaded)==2); // pendientes en vuelo aunque el journal ya se confirmo
  assert(afterAck.clear(loaded)&&!loaded.activo);assert(pendingMeasurements(j,loaded)==0);EnvioPersistente done;assert(done.begin(loaded)&&!loaded.activo);
  // Fallo al guardar no confirma ni publica una muestra nueva.
  FakeNvs::writes.clear();FakeNvs::failAt=1;assert(!done.save(f));FakeNvs::failAt=-1;
  EnvioPersistente failed;assert(failed.begin(loaded)&&!loaded.activo);
  assert(failed.save(f));FakeNvs::data["iciv_aura4/vuelo"][30]^=1;
  EnvioPersistente corrupt;assert(!corrupt.begin(loaded)); // no borra un registro corrupto
  std::cout<<"Envio persistente: reinicios, JSON/ID estables, ACK agrupado, fallo y CRC OK\n";
}
