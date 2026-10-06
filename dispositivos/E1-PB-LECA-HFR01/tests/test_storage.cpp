#include "fakes/Preferences.h"
#include "../mesh_storage.h"
#include <cassert>
#include <iostream>
Muestra sample(int n){Muestra m={};m.id[0]=n;m.temp100=400;m.flags=VALID;return m;}
int main(){
 FakeNvs::reset();Journal j;MeshStorage store;assert(store.begin(j));
 for(int n=1;n<=5;++n){pushSample(j,sample(n));FakeNvs::writes.clear();assert(store.save(j));assert(FakeNvs::writes.size()==2);assert(FakeNvs::writes[0].second==sizeof(StoredRecord));}
 assert(j.count[0]==5);FakeNvs::writes.clear();assert(acknowledge(j,sample(1).id));assert(store.save(j));assert(FakeNvs::writes.size()==1&&FakeNvs::writes[0].second==sizeof(QueueIndex));
 Journal restored;MeshStorage reboot;assert(reboot.begin(restored));assert(restored.count[0]==4);Muestra next;assert(nextSample(restored,next)&&next.id[0]==2);
 // Corte entre escritura de registro e indice: reinicio recupera cola anterior.
 pushSample(j,sample(6));FakeNvs::writes.clear();FakeNvs::failAt=2;assert(!store.save(j));FakeNvs::failAt=-1;
 MeshStorage crashed;assert(crashed.begin(restored));assert(restored.count[0]==4);assert(store.save(j));
 // Indice final corrupto: elegir el indice anterior completo y valido.
 const auto lastIndex=FakeNvs::writes.back().first;FakeNvs::data[lastIndex][0]^=1;
 MeshStorage fallback;assert(fallback.begin(restored));assert(restored.count[0]==4);
 // Recuperacion repetida, overflow y primera lectura protegida persistente.
 FakeNvs::reset();MeshStorage full;assert(full.begin(j));auto first=sample(99);first.flags|=POWER_FIRST;assert(protectFirst(j,first));
 for(int n=1;n<=70;++n){pushSample(j,sample(n));assert(full.save(j));}
 assert(j.count[0]==30&&j.dropped[0]==40);MeshStorage fullReboot;assert(fullReboot.begin(restored));assert(nextSample(restored,next)&&next.id[0]==99);
 assert(acknowledge(restored,first.id));assert(fullReboot.save(restored));MeshStorage afterAck;assert(afterAck.begin(j));assert(j.firstAcked[0]&&j.first[0].id[0]==99);
 // Inicio del corte queda separado incluso si ninguna sonda responde.
 j.firstPower=sample(98);j.firstPower.kind=ALERTA_ENERGIA;j.firstPower.state=1;j.firstPower.temp100=TEMP_INVALIDA;j.firstPower.flags=BATTERY|POWER_KNOWN;j.hasPower=1;
 assert(afterAck.save(j));MeshStorage powerReboot;assert(powerReboot.begin(restored));assert(nextSample(restored,next)&&next.id[0]==98);assert(acknowledge(restored,next.id));assert(powerReboot.save(restored));
 MeshStorage powerAck;assert(powerAck.begin(j));assert(j.hasPower&&j.powerAcked&&j.firstPower.id[0]==98);
 // Cola antigua no se borra ni interpreta con el formato nuevo.
 FakeNvs::reset();FakeNvs::data["iciv_mesh/q0"]={1,2,3};MeshStorage legacy;assert(!legacy.begin(j));assert(FakeNvs::data["iciv_mesh/q0"].size()==3);
 std::cout<<"NVS: registros, ACK sin reescribir FIFO, reinicios, corte, CRC, overflow y legado OK; indice="<<sizeof(QueueIndex)<<", registro="<<sizeof(StoredRecord)<<" bytes\n";
}
