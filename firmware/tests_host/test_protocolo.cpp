#include "../comun/protocolo_aura.h"
#include "aserciones.h"

int main() {
  const uint8_t A[6] = {0xAA,0,0,0,0,1};
  const uint8_t B[6] = {0xBB,0,0,0,0,2};

  // La trama entra en el limite de ESP-NOW
  VERIFICAR(sizeof(TramaAura) == 197);
  VERIFICAR(sizeof(TramaAura) <= 250);
  VERIFICAR(AURA_CABECERA_BYTES == 17);

  // init deja la trama consistente
  TramaAura t;
  const uint8_t datos[3] = {10, 20, 30};
  aura_trama_init(&t, AURA_TIPO_TELEMETRIA, A, B, 7, datos, 3);
  VERIFICAR(t.version == AURA_PROTO_VERSION);
  VERIFICAR(t.tipo == AURA_TIPO_TELEMETRIA);
  VERIFICAR(t.seq == 7);
  VERIFICAR(t.largo == 3);
  VERIFICAR(memcmp(t.mac_origen, A, 6) == 0);
  VERIFICAR(memcmp(t.mac_destino, B, 6) == 0);
  VERIFICAR(t.payload[2] == 30);

  // Solo se transmiten los bytes utiles, no los 197 completos
  VERIFICAR(aura_trama_bytes(&t) == 20);

  // Validacion de lo que llega por la radio
  VERIFICAR(aura_trama_valida((const uint8_t*)&t, 20) == true);
  VERIFICAR(aura_trama_valida((const uint8_t*)&t, 16) == false);  // mas corta que la cabecera
  VERIFICAR(aura_trama_valida((const uint8_t*)&t, 19) == false);  // largo no coincide

  TramaAura mala = t;
  mala.version = 99;
  VERIFICAR(aura_trama_valida((const uint8_t*)&mala, 20) == false);

  TramaAura larga = t;
  larga.largo = 200;  // mayor que AURA_PAYLOAD_MAX
  VERIFICAR(aura_trama_valida((const uint8_t*)&larga, 20) == false);

  // Direccionamiento: destino final, no proximo salto
  VERIFICAR(aura_es_para_mi(&t, B) == true);
  VERIFICAR(aura_es_para_mi(&t, A) == false);

  RESUMEN();
}
