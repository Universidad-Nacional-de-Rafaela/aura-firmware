#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../../comun/protocolo_aura.h"

// Decisiones del gateway que no dependen de la placa, para probarlas en host.

// ===== Criterio de persistencia del REST (contrato §6) =====
// El endpoint devuelve 201 aunque todos los eventos hayan fallado: hay que
// mirar los contadores. Un duplicado cuenta como persistido (ya estaba en la
// base). Un campo ausente en la respuesta llega como -1 y no persiste nada.
static inline bool gw_rest_persistio(int http, long inserted, long duplicates,
                                     long errors, long enviados) {
  if (http != 201) return false;
  if (inserted < 0 || duplicates < 0 || errors != 0) return false;
  return inserted + duplicates == enviados;
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

// ===== Comandos permitidos por tipo de nodo (contrato §3.3) =====
// lista es CSV sin espacios ("set_config,reiniciar"). Coincidencia exacta de
// un elemento: "set" no habilita "set_config".
static inline bool gw_comando_permitido(const char* lista, const char* comando) {
  if (lista == NULL || comando == NULL || comando[0] == '\0') return false;
  size_t n = strlen(comando);
  const char* p = lista;
  while (*p) {
    const char* fin = strchr(p, ',');
    size_t largo = fin ? (size_t)(fin - p) : strlen(p);
    if (largo == n && strncmp(p, comando, n) == 0) return true;
    if (!fin) break;
    p = fin + 1;
  }
  return false;
}
