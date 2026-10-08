#include "../protocolo_aura.h"
#include "aserciones.h"

static MuestraAura muestra(const char* json, uint32_t ts) {
  MuestraAura m;
  memset(&m, 0, sizeof(m));
  for (int i = 0; i < AURA_INGEST_ID_BYTES; i++) m.ingest_id[i] = (uint8_t)(0x10 + i);
  m.ts = ts;
  m.largo = (uint8_t)strlen(json);
  memcpy(m.values, json, m.largo);
  return m;
}

int main() {
  // Cabecera sin MAC: origen y destino los da ESP-WIFI-MESH (v3)
  VERIFICAR(sizeof(TramaAura) == 185);
  VERIFICAR(AURA_CABECERA_BYTES == 5);
  VERIFICAR(AURA_PROTO_VERSION == 3);

  // Los valores de los tipos viajan por aire: no se renumeran
  VERIFICAR(AURA_TIPO_TELEMETRIA == 1);
  VERIFICAR(AURA_TIPO_COMANDO == 2);
  VERIFICAR(AURA_TIPO_ACK == 3);
  VERIFICAR(AURA_TIPO_CONFIRMACION == 4);
  VERIFICAR(AURA_TIPO_RESULTADO == 5);
  VERIFICAR(AURA_TIPO_REPORTE == 6);
  VERIFICAR(AURA_TIPO_ALERTA == 7);
  VERIFICAR(AURA_TIPO_PING == 8);

  // init deja la trama consistente
  TramaAura t;
  const uint8_t datos[3] = {10, 20, 30};
  aura_trama_init(&t, AURA_TIPO_TELEMETRIA, 7, datos, 3);
  VERIFICAR(t.version == AURA_PROTO_VERSION);
  VERIFICAR(t.tipo == AURA_TIPO_TELEMETRIA);
  VERIFICAR(t.seq == 7);
  VERIFICAR(t.largo == 3);
  VERIFICAR(t.payload[2] == 30);

  // Solo se transmiten los bytes utiles, no los 185 completos
  VERIFICAR(aura_trama_bytes(&t) == 8);

  // Validacion de lo que llega por la radio
  VERIFICAR(aura_trama_valida((const uint8_t*)&t, 8) == true);
  VERIFICAR(aura_trama_validar((const uint8_t*)&t, 8) == AURA_TRAMA_OK);
  VERIFICAR(aura_trama_valida((const uint8_t*)&t, 4) == false);  // mas corta que la cabecera
  VERIFICAR(aura_trama_validar((const uint8_t*)&t, 4) == AURA_TRAMA_CORTA);
  VERIFICAR(aura_trama_valida((const uint8_t*)&t, 7) == false);  // largo no coincide
  VERIFICAR(aura_trama_validar((const uint8_t*)&t, 7) == AURA_TRAMA_LARGO);
  VERIFICAR(aura_trama_validar(NULL, 8) == AURA_TRAMA_CORTA);

  // Una trama v2 (mesh ESP-NOW) se distingue de una rota: se cuenta aparte
  TramaAura vieja = t;
  vieja.version = 2;
  VERIFICAR(aura_trama_valida((const uint8_t*)&vieja, 8) == false);
  VERIFICAR(aura_trama_validar((const uint8_t*)&vieja, 8) == AURA_TRAMA_VERSION);

  TramaAura larga = t;
  larga.largo = 200;  // mayor que AURA_PAYLOAD_MAX
  VERIFICAR(aura_trama_valida((const uint8_t*)&larga, 8) == false);

  // Sentido de cada tipo: el raiz decide que publicar con esto
  VERIFICAR(aura_tipo_sube(AURA_TIPO_TELEMETRIA));
  VERIFICAR(aura_tipo_sube(AURA_TIPO_RESULTADO));
  VERIFICAR(aura_tipo_sube(AURA_TIPO_REPORTE));
  VERIFICAR(aura_tipo_sube(AURA_TIPO_ALERTA));
  VERIFICAR(!aura_tipo_sube(AURA_TIPO_COMANDO));
  VERIFICAR(!aura_tipo_sube(AURA_TIPO_CONFIRMACION));
  VERIFICAR(aura_tipo_baja(AURA_TIPO_COMANDO));
  VERIFICAR(aura_tipo_baja(AURA_TIPO_CONFIRMACION));
  VERIFICAR(!aura_tipo_baja(AURA_TIPO_TELEMETRIA));
  VERIFICAR(!aura_tipo_baja(AURA_TIPO_ACK));
  VERIFICAR(!aura_tipo_sube(AURA_TIPO_ACK));
  VERIFICAR(!aura_tipo_sube(AURA_TIPO_PING));

  // Telemetria: ingest_id + ts + JSON, ida y vuelta
  VERIFICAR(AURA_VALUES_MAX == 160);
  MuestraAura m = muestra("{\"temp_c\":4.5}", 1790000000u);
  TramaAura tt;
  VERIFICAR(aura_telemetria_armar(&tt, 9, &m) == true);
  VERIFICAR(tt.tipo == AURA_TIPO_TELEMETRIA);
  VERIFICAR(tt.largo == 20 + 14);
  VERIFICAR(aura_trama_valida((const uint8_t*)&tt, aura_trama_bytes(&tt)));
  // ts en little-endian explicito, no depende de la alineacion
  VERIFICAR(tt.payload[16] == 0x80 && tt.payload[17] == 0x3B && tt.payload[19] == 0x6A);

  MuestraAura leida;
  VERIFICAR(aura_telemetria_leer(&tt, &leida) == true);
  VERIFICAR(memcmp(leida.ingest_id, m.ingest_id, AURA_INGEST_ID_BYTES) == 0);
  VERIFICAR(leida.ts == 1790000000u);
  VERIFICAR(leida.largo == 14);
  VERIFICAR(memcmp(leida.values, "{\"temp_c\":4.5}", 14) == 0);

  // values de exactamente 160 B entra; uno mas no
  char json160[AURA_VALUES_MAX + 2];
  memset(json160, 'x', sizeof(json160));
  json160[AURA_VALUES_MAX] = '\0';
  MuestraAura grande = muestra(json160, 0);
  VERIFICAR(aura_telemetria_armar(&tt, 1, &grande) == true);
  VERIFICAR(tt.largo == AURA_PAYLOAD_MAX);
  grande.largo = AURA_VALUES_MAX + 1;
  VERIFICAR(aura_telemetria_armar(&tt, 1, &grande) == false);

  // Leer una telemetria sin cabecera completa o de otro tipo falla
  TramaAura corta;
  aura_trama_init(&corta, AURA_TIPO_TELEMETRIA, 1, datos, 3);
  VERIFICAR(aura_telemetria_leer(&corta, &leida) == false);
  aura_trama_init(&corta, AURA_TIPO_COMANDO, 1, NULL, 0);
  VERIFICAR(aura_telemetria_leer(&corta, &leida) == false);

  // Confirmacion: ingest_id + hora del gateway
  TramaAura c;
  aura_confirmacion_armar(&c, 3, m.ingest_id, 1790000123u);
  VERIFICAR(c.tipo == AURA_TIPO_CONFIRMACION);
  VERIFICAR(c.largo == AURA_CONFIRMACION_BYTES);
  uint8_t id[AURA_INGEST_ID_BYTES];
  uint32_t hora = 0;
  VERIFICAR(aura_confirmacion_leer(&c, id, &hora) == true);
  VERIFICAR(memcmp(id, m.ingest_id, AURA_INGEST_ID_BYTES) == 0);
  VERIFICAR(hora == 1790000123u);
  VERIFICAR(aura_confirmacion_leer(&tt, id, &hora) == false);

  // Payload como texto: siempre terminado en cero, recortado al buffer
  TramaAura j;
  aura_trama_init(&j, AURA_TIPO_REPORTE, 1, (const uint8_t*)"{\"a\":1}", 7);
  char texto[8];
  aura_payload_texto(&j, texto, sizeof(texto));
  VERIFICAR(strcmp(texto, "{\"a\":1}") == 0);
  char chico[4];
  aura_payload_texto(&j, chico, sizeof(chico));
  VERIFICAR(strcmp(chico, "{\"a") == 0);

  // Hora valida: antes de 2026 es un RTC sin ajustar
  VERIFICAR(!aura_hora_valida(0));
  VERIFICAR(!aura_hora_valida(1700000000u));
  VERIFICAR(aura_hora_valida(1790000000u));

  RESUMEN();
}
