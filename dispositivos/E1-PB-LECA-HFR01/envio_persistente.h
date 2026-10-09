#pragma once
#include <Preferences.h>
#include "modelo_envio.h"
// Copia NVS del mensaje completo en vuelo: si se corta entre el ACK de la
// biblioteca y el guardado del journal, se reenvia el MISMO UUID y JSON.
class EnvioPersistente {
  Preferences prefs;
public:
  bool begin(EnvioProtegido& out) {
    out={};
    if (!prefs.begin("iciv_aura4",false)) return false;
    if (!prefs.isKey("vuelo")) return true;
    if (prefs.getBytesLength("vuelo")!=sizeof(out) || prefs.getBytes("vuelo",&out,sizeof(out))!=sizeof(out)) return false;
    return out.magic==0x49434134 && out.activo<=1 && out.muestra.largo<=AURA_VALUES_MAX &&
      out.control==crc32(reinterpret_cast<const uint8_t*>(&out),offsetof(EnvioProtegido,control));
  }
  bool save(const EnvioProtegido& out) {
    if (prefs.putBytes("vuelo",&out,sizeof(out))!=sizeof(out)) return false;
    EnvioProtegido check={};
    return prefs.getBytes("vuelo",&check,sizeof(check))==sizeof(check) && !memcmp(&check,&out,sizeof(out));
  }
  bool clear(EnvioProtegido& out) {
    EnvioProtegido empty={}; empty.magic=0x49434134;
    empty.control=crc32(reinterpret_cast<const uint8_t*>(&empty),offsetof(EnvioProtegido,control));
    if (!save(empty)) return false;
    out=empty; return true;
  }
};
