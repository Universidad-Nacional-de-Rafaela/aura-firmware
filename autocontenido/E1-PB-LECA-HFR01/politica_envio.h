#pragma once
#include <stdint.h>
// Inicial + tres reintentos de datos. La escucha/reconexion de mesh no se apaga.
struct PoliticaEnvio {
  uint8_t intentos=0;
  bool lenta=false;
  uint32_t ultimo=0,proximo=0;
  void confirmar() { intentos=0; lenta=false; }
  void revisar(uint32_t now,uint32_t espera,uint32_t recuperacion) {
    if(!lenta && intentos>=4 && uint32_t(now-ultimo)>=espera) {
      lenta=true; proximo=now+recuperacion;
    }
  }
  void intento(uint32_t now,bool aceptado,uint32_t recuperacion) {
    ultimo=now; if(intentos<4) ++intentos;
    if(lenta) proximo=now+recuperacion;
    else if(!aceptado) revisar(now,0,recuperacion);
  }
  bool corresponde(uint32_t now) const { return !lenta || int32_t(now-proximo)>=0; }
};
