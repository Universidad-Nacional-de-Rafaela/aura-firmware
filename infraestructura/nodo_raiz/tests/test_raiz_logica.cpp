#include "../raiz_logica.h"
#include "aserciones.h"

int main() {
  // ===== hw_id de una MAC: "mac-" + 12 hex en minusculas (contrato v4.0) =====
  const uint8_t mac[6] = {0xE0, 0x72, 0xA1, 0xF7, 0xEF, 0xE4};
  char hw[RAIZ_HW_ID_BYTES];
  raiz_hw_id_de_mac(mac, hw);
  VERIFICAR(strcmp(hw, "mac-e072a1f7efe4") == 0);
  VERIFICAR(RAIZ_HW_ID_BYTES == 17);

  // ===== Topicos hw/<hw_id>/<sufijo> =====
  char top[RAIZ_TOPICO_MAX];
  VERIFICAR(raiz_topico(top, sizeof(top), hw, "data") == true);
  VERIFICAR(strcmp(top, "hw/mac-e072a1f7efe4/data") == 0);
  VERIFICAR(raiz_topico(top, sizeof(top), hw, "alerts/sensor") == true);
  VERIFICAR(strcmp(top, "hw/mac-e072a1f7efe4/alerts/sensor") == 0);
  char chico[10];
  VERIFICAR(raiz_topico(chico, sizeof(chico), hw, "data") == false);  // no entra: no se trunca

  // ===== Lo que consume el raiz: solo hw/mac-<12 hex>/{ack,command} =====
  uint8_t m[6];
  char tipo[RAIZ_TIPO_MAX];
  VERIFICAR(raiz_parsear_topico("hw/mac-e072a1f7efe4/ack", m, tipo) == true);
  VERIFICAR(memcmp(m, mac, 6) == 0);
  VERIFICAR(strcmp(tipo, "ack") == 0);
  VERIFICAR(raiz_parsear_topico("hw/mac-e072a1f7efe4/command", m, tipo) == true);
  VERIFICAR(strcmp(tipo, "command") == 0);
  // Ajenos: se ignoran sin mandar nada a la mesh
  VERIFICAR(raiz_parsear_topico("hw/MAC-E072A1F7EFE4/ack", m, tipo) == false);      // mayusculas
  VERIFICAR(raiz_parsear_topico("hw/mac-E072A1F7EFE4/ack", m, tipo) == false);
  VERIFICAR(raiz_parsear_topico("hw/eui-798a381dd8c1cba4/command", m, tipo) == false);  // LoRaWAN
  VERIFICAR(raiz_parsear_topico("hw/svc-lorawan-bridge/status", m, tipo) == false);
  VERIFICAR(raiz_parsear_topico("hw/mac-e072a1f7efe4/data", m, tipo) == false);     // no lo consume
  VERIFICAR(raiz_parsear_topico("hw/mac-e072a1f7efe4/ack/extra", m, tipo) == false);
  VERIFICAR(raiz_parsear_topico("hw/mac-e072a1f7ef/ack", m, tipo) == false);        // MAC corta
  VERIFICAR(raiz_parsear_topico("hw/mac-e072a1f7efe4a/ack", m, tipo) == false);     // MAC larga
  VERIFICAR(raiz_parsear_topico("hw/mac-e072a1f7efgz/ack", m, tipo) == false);      // no es hex
  VERIFICAR(raiz_parsear_topico("devices/650e8400-e29b-41d4-a716-446655440001/command", m, tipo) == false);
  VERIFICAR(raiz_parsear_topico("", m, tipo) == false);
  VERIFICAR(raiz_parsear_topico(NULL, m, tipo) == false);

  // ===== ingest_id desde el texto del ack =====
  uint8_t id[AURA_INGEST_ID_BYTES];
  VERIFICAR(raiz_ingest_id_de_texto("5f0c1b1e-8a6d-4a55-9f2b-7c3e2d1a0b99", id) == true);
  VERIFICAR(id[0] == 0x5f && id[1] == 0x0c && id[15] == 0x99);
  VERIFICAR(raiz_ingest_id_de_texto("5F0C1B1E-8A6D-4A55-9F2B-7C3E2D1A0B99", id) == true);
  VERIFICAR(id[0] == 0x5f);
  VERIFICAR(raiz_ingest_id_de_texto("5f0c1b1e8a6d4a559f2b7c3e2d1a0b99", id) == false);      // sin guiones
  VERIFICAR(raiz_ingest_id_de_texto("5f0c1b1e-8a6d-4a55-9f2b-7c3e2d1a0b9", id) == false);   // corto
  VERIFICAR(raiz_ingest_id_de_texto("5f0c1b1e-8a6d-4a55-9f2b-7c3e2d1a0b999", id) == false); // largo
  VERIFICAR(raiz_ingest_id_de_texto("5f0c1b1e-8a6d-4a55-9f2b-7c3e2d1a0bzz", id) == false);  // no hex
  VERIFICAR(raiz_ingest_id_de_texto("", id) == false);
  VERIFICAR(raiz_ingest_id_de_texto(NULL, id) == false);

  // ===== Que ack confirma la muestra al nodo (contrato §3.6) =====
  VERIFICAR(raiz_ack_confirma("persistido") == true);
  VERIFICAR(raiz_ack_confirma("duplicado") == true);
  VERIFICAR(raiz_ack_confirma("cuarentena") == true);
  VERIFICAR(raiz_ack_confirma("descartado") == true);
  VERIFICAR(raiz_ack_confirma("rechazado") == true);
  // Un resultado desconocido NO confirma: la muestra se reintenta, nunca se borra
  VERIFICAR(raiz_ack_confirma("Persistido") == false);
  VERIFICAR(raiz_ack_confirma("ok") == false);
  VERIFICAR(raiz_ack_confirma("persistido ") == false);
  VERIFICAR(raiz_ack_confirma("") == false);
  VERIFICAR(raiz_ack_confirma(NULL) == false);

  // ===== Tipo de alerta: lo que acepta el backend en hw/<hw_id>/alerts/<tipo> =====
  VERIFICAR(raiz_alerta_tipo_valido("sensor") == true);
  VERIFICAR(raiz_alerta_tipo_valido("energia") == true);
  VERIFICAR(raiz_alerta_tipo_valido("sonda_freezer") == true);
  VERIFICAR(raiz_alerta_tipo_valido("Sensor") == false);
  VERIFICAR(raiz_alerta_tipo_valido("sensor/x") == false);
  VERIFICAR(raiz_alerta_tipo_valido("") == false);
  VERIFICAR(raiz_alerta_tipo_valido(NULL) == false);
  VERIFICAR(raiz_alerta_tipo_valido("abcdefghijklmnopqrstuvwxy") == false);  // 25 > 24

  // ===== Offline inferido: 3 x intervalo sin tramas, una sola vez =====
  VERIFICAR(gw_debe_marcar_offline(1000 + 179999, 1000, 60, false) == false);
  VERIFICAR(gw_debe_marcar_offline(1000 + 180001, 1000, 60, false) == true);
  VERIFICAR(gw_debe_marcar_offline(1000 + 180001, 1000, 60, true) == false);  // ya publicado
  VERIFICAR(gw_debe_marcar_offline(999999999, 1000, 0, false) == false);      // sin reporte
  VERIFICAR(gw_debe_marcar_offline(100, 0xFFFFFF00u, 60, false) == false);    // vuelta de millis()
  VERIFICAR(gw_debe_marcar_offline(180000, 0xFFFFFF00u, 60, false) == true);
  VERIFICAR(gw_debe_marcar_offline(3u * 86400u * 1000u - 1, 0, 86400, false) == false);

  // ===== Epoca -> ISO 8601 UTC, para "ts" =====
  char iso[GW_ISO_BYTES];
  VERIFICAR(gw_epoca_a_iso(1767225600u, iso) == true);
  VERIFICAR(strcmp(iso, "2026-01-01T00:00:00Z") == 0);
  VERIFICAR(gw_epoca_a_iso(1791209580u, iso) == true);
  VERIFICAR(strcmp(iso, "2026-10-05T14:13:00Z") == 0);
  VERIFICAR(gw_epoca_a_iso(1835481599u, iso) == true);
  VERIFICAR(strcmp(iso, "2028-02-29T23:59:59Z") == 0);  // bisiesto
  VERIFICAR(gw_epoca_a_iso(4102444800u, iso) == true);
  VERIFICAR(strcmp(iso, "2100-01-01T00:00:00Z") == 0);  // 2100 no es bisiesto
  VERIFICAR(gw_epoca_a_iso(0, iso) == false);           // sin hora: se omite ts
  VERIFICAR(gw_epoca_a_iso(1700000000u, iso) == false);

  RESUMEN();
}
