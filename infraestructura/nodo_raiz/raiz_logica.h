#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../../comun/protocolo_aura.h"

// Decisiones del raiz de la mesh que no dependen de la placa, para probarlas
// en host. El raiz es el adaptador de la mesh en el sentido del contrato v4.0:
// publica en hw/<hw_id>/... con el hw_id de cada placa, y no conoce UUID.

// ===== hw_id de una placa de la mesh =====
// "mac-" + la MAC de fabrica en hexadecimal minuscula, sin separadores.
#define RAIZ_HW_ID_BYTES 17   // "mac-" + 12 + terminador
#define RAIZ_TOPICO_MAX  64   // "hw/" + hw_id + "/alerts/" + tipo (24) + terminador
#define RAIZ_TIPO_MAX    8    // "command" + terminador

static inline void raiz_hw_id_de_mac(const uint8_t mac[6], char salida[RAIZ_HW_ID_BYTES]) {
  snprintf(salida, RAIZ_HW_ID_BYTES, "mac-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// "hw/<hw_id>/<sufijo>". false si no entra: un topico truncado publicaria en
// otro lado sin avisar.
static inline bool raiz_topico(char* salida, size_t cap, const char* hw_id, const char* sufijo) {
  int n = snprintf(salida, cap, "hw/%s/%s", hw_id, sufijo);
  return n > 0 && (size_t)n < cap;
}

static inline int raiz_hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Lo unico que consume el raiz: hw/mac-<12 hex minusculas>/ack y .../command.
// Cualquier otra cosa (LoRaWAN, servicios, mayusculas, niveles de mas) es de
// otro adaptador o esta mal armada, y no se manda nada a la mesh.
static inline bool raiz_parsear_topico(const char* topico, uint8_t mac[6], char tipo[RAIZ_TIPO_MAX]) {
  if (topico == NULL || strncmp(topico, "hw/mac-", 7) != 0) return false;
  const char* p = topico + 7;
  for (int i = 0; i < 12; i++) {
    char c = p[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  if (p[12] != '/') return false;
  const char* resto = p + 13;
  if (strcmp(resto, "ack") != 0 && strcmp(resto, "command") != 0) return false;
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(raiz_hex(p[2 * i]) << 4 | raiz_hex(p[2 * i + 1]));
  strcpy(tipo, resto);
  return true;
}

// "5f0c1b1e-8a6d-4a55-9f2b-7c3e2d1a0b99" -> 16 bytes. Solo el formato con
// guiones, que es el que arma el nodo (aura_ingest_id_texto) y devuelve AURA.
static inline bool raiz_ingest_id_de_texto(const char* s, uint8_t id[AURA_INGEST_ID_BYTES]) {
  if (s == NULL || strlen(s) != 36) return false;
  int b = 0;
  for (int i = 0; i < 36; ) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-') return false;
      i++;
      continue;
    }
    int hi = raiz_hex(s[i]), lo = raiz_hex(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    id[b++] = (uint8_t)(hi << 4 | lo);
    i += 2;
  }
  return b == AURA_INGEST_ID_BYTES;
}

// ===== Que ack confirma la muestra al nodo (contrato §3.6) =====
// Los cinco resultados confirman: la muestra ya no tiene que reintentarse.
// Cualquier otro valor NO confirma: ante la duda, el nodo reintenta. Borrar
// una muestra por un ack mal escrito seria perderla.
static inline bool raiz_ack_confirma(const char* resultado) {
  if (resultado == NULL) return false;
  return strcmp(resultado, "persistido") == 0 || strcmp(resultado, "duplicado") == 0 ||
         strcmp(resultado, "cuarentena") == 0 || strcmp(resultado, "descartado") == 0 ||
         strcmp(resultado, "rechazado") == 0;
}

// ===== Tipo de alerta =====
// El backend solo reconoce hw/<hw_id>/alerts/[a-z_]{1,24}.
static inline bool raiz_alerta_tipo_valido(const char* t) {
  if (t == NULL) return false;
  size_t n = strlen(t);
  if (n == 0 || n > 24) return false;
  for (size_t i = 0; i < n; i++)
    if (!((t[i] >= 'a' && t[i] <= 'z') || t[i] == '_')) return false;
  return true;
}

// ===== Offline inferido (contrato §3.2) =====
// Un nodo que pasa 3 x su intervalo sin mandar nada se publica offline, una
// sola vez. Sin reporte no hay intervalo conocido (0) y no se infiere nada.
// La resta sin signo resuelve la vuelta de millis() a los 49 dias.
static inline bool gw_debe_marcar_offline(uint32_t ahora_ms, uint32_t ultimo_ms,
                                          uint32_t intervalo_s, bool ya_offline) {
  if (ya_offline || intervalo_s == 0) return false;
  uint64_t limite = 3ull * intervalo_s * 1000ull;
  return (uint64_t)(uint32_t)(ahora_ms - ultimo_ms) > limite;
}

// ===== Epoca -> ISO 8601 UTC =====
// "2026-10-05T14:03:00Z": 20 caracteres + terminador. Sin gmtime(): es la
// misma cuenta en la placa y en los tests. Devuelve false si la hora no es
// valida, y entonces el evento va sin "ts" (no se inventa una hora).
#define GW_ISO_BYTES 21

static inline bool gw_epoca_a_iso(uint32_t epoca, char salida[GW_ISO_BYTES]) {
  if (!aura_hora_valida(epoca)) return false;
  uint32_t dias = epoca / 86400u, seg = epoca % 86400u;
  // Dias desde 1970 -> fecha civil (algoritmo de Howard Hinnant).
  int64_t z = (int64_t)dias + 719468;
  int64_t era = z / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  int64_t y = (int64_t)yoe + era * 400 + (m <= 2 ? 1 : 0);
  // Buffer holgado: el compilador no puede saber que cada campo entra en dos
  // digitos, y con -Werror=format-truncation no compila sobre salida directo.
  char tmp[48];
  snprintf(tmp, sizeof(tmp), "%04d-%02u-%02uT%02u:%02u:%02uZ",
           (int)y, (unsigned)m, (unsigned)d,
           (unsigned)(seg / 3600), (unsigned)(seg / 60 % 60), (unsigned)(seg % 60));
  memcpy(salida, tmp, GW_ISO_BYTES - 1);
  salida[GW_ISO_BYTES - 1] = '\0';
  return true;
}
