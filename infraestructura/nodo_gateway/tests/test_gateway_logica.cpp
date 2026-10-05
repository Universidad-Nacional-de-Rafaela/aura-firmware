#include "../gateway_logica.h"
#include "aserciones.h"

int main() {
  // Criterio de persistencia del contrato §6, con un evento por request:
  // 201 y errors == 0 y inserted + duplicates == enviados.
  VERIFICAR(gw_rest_persistio(201, 1, 0, 0, 1) == true);
  VERIFICAR(gw_rest_persistio(201, 0, 1, 0, 1) == true);   // duplicado = ya estaba
  VERIFICAR(gw_rest_persistio(201, 0, 0, 1, 1) == false);  // el endpoint da 201 con errores
  VERIFICAR(gw_rest_persistio(201, 1, 0, 1, 1) == false);
  VERIFICAR(gw_rest_persistio(201, 0, 0, 0, 1) == false);  // no conto nada
  VERIFICAR(gw_rest_persistio(200, 1, 0, 0, 1) == false);  // solo 201
  VERIFICAR(gw_rest_persistio(500, 1, 0, 0, 1) == false);
  VERIFICAR(gw_rest_persistio(-1, 1, 0, 0, 1) == false);   // sin conexion (HTTPClient)
  VERIFICAR(gw_rest_persistio(201, 2, 0, 0, 1) == false);  // conto de mas: no se confia
  VERIFICAR(gw_rest_persistio(201, -1, 2, 0, 1) == false); // campo ausente (-1)

  // Offline inferido: 3 x intervalo sin tramas, una sola vez
  VERIFICAR(gw_debe_marcar_offline(1000 + 179999, 1000, 60, false) == false);
  VERIFICAR(gw_debe_marcar_offline(1000 + 180001, 1000, 60, false) == true);
  VERIFICAR(gw_debe_marcar_offline(1000 + 180001, 1000, 60, true) == false);  // ya publicado
  VERIFICAR(gw_debe_marcar_offline(999999999, 1000, 0, false) == false);      // sin reporte: no se infiere
  // millis() da la vuelta a los 49 dias: la resta sin signo lo resuelve
  VERIFICAR(gw_debe_marcar_offline(100, 0xFFFFFF00u, 60, false) == false);
  VERIFICAR(gw_debe_marcar_offline(180000, 0xFFFFFF00u, 60, false) == true);
  // intervalo maximo (1 dia) sin desbordar 32 bits
  VERIFICAR(gw_debe_marcar_offline(3u * 86400u * 1000u - 1, 0, 86400, false) == false);

  // Epoca -> ISO 8601 UTC, para "ts" del REST
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

  // Comandos permitidos por tipo de nodo: token exacto en una lista CSV
  VERIFICAR(gw_comando_permitido("set_config", "set_config") == true);
  VERIFICAR(gw_comando_permitido("set_config,reiniciar", "reiniciar") == true);
  VERIFICAR(gw_comando_permitido("set_config,reiniciar", "set_config") == true);
  VERIFICAR(gw_comando_permitido("set_config", "set") == false);
  VERIFICAR(gw_comando_permitido("set_config", "set_config2") == false);
  VERIFICAR(gw_comando_permitido("set_config,reiniciar", "config,reiniciar") == false);
  VERIFICAR(gw_comando_permitido("", "set_config") == false);
  VERIFICAR(gw_comando_permitido(NULL, "set_config") == false);
  VERIFICAR(gw_comando_permitido("set_config", "") == false);
  VERIFICAR(gw_comando_permitido("set_config", NULL) == false);

  RESUMEN();
}
