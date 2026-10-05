#include "../nodo_mesh_logica.h"
#include "aserciones.h"

int main() {
  EstadoSondas e;
  aura_sondas_init(&e);

  // Una sonda que anda bien no genera alerta, ni la primera vez ni despues
  VERIFICAR(aura_sonda_cambio(&e, "temp_heladera_c", true) == AURA_SONDA_SIN_CAMBIO);
  VERIFICAR(aura_sonda_cambio(&e, "temp_heladera_c", true) == AURA_SONDA_SIN_CAMBIO);

  // Falla: una alerta, no una por medicion (contrato §3.5)
  VERIFICAR(aura_sonda_cambio(&e, "temp_heladera_c", false) == AURA_SONDA_FALLO);
  for (int i = 0; i < 60; i++)
    VERIFICAR(aura_sonda_cambio(&e, "temp_heladera_c", false) == AURA_SONDA_SIN_CAMBIO);

  // Vuelve: una alerta de recuperada
  VERIFICAR(aura_sonda_cambio(&e, "temp_heladera_c", true) == AURA_SONDA_RECUPERADA);
  VERIFICAR(aura_sonda_cambio(&e, "temp_heladera_c", true) == AURA_SONDA_SIN_CAMBIO);

  // Una sonda que arranca fallando avisa en la primera medicion
  VERIFICAR(aura_sonda_cambio(&e, "temp_freezer_c", false) == AURA_SONDA_FALLO);
  VERIFICAR(aura_sonda_cambio(&e, "temp_freezer_c", false) == AURA_SONDA_SIN_CAMBIO);

  // Cada sonda lleva su propio estado
  VERIFICAR(aura_sonda_cambio(&e, "temp_heladera_c", false) == AURA_SONDA_FALLO);
  VERIFICAR(aura_sonda_cambio(&e, "temp_freezer_c", true) == AURA_SONDA_RECUPERADA);

  // Tabla llena: una sonda nueva no se puede seguir y no genera alertas
  // (para no mandar una por medicion); el nodo lo informa por serie.
  EstadoSondas llena;
  aura_sondas_init(&llena);
  char nombre[16];
  for (int i = 0; i < AURA_SONDAS_MAX; i++) {
    snprintf(nombre, sizeof(nombre), "s%d", i);
    aura_sonda_cambio(&llena, nombre, true);
  }
  VERIFICAR(aura_sonda_cambio(&llena, "otra", false) == AURA_SONDA_SIN_LUGAR);
  VERIFICAR(aura_sonda_cambio(&llena, "otra", false) == AURA_SONDA_SIN_LUGAR);
  VERIFICAR(aura_sonda_cambio(&llena, "s0", false) == AURA_SONDA_FALLO);

  // Nombre mas largo que el lugar: se compara entero, no recortado
  EstadoSondas largos;
  aura_sondas_init(&largos);
  const char* a = "un_nombre_de_campo_larguisimo_a";
  const char* b = "un_nombre_de_campo_larguisimo_b";
  VERIFICAR(aura_sonda_cambio(&largos, a, false) == AURA_SONDA_SIN_LUGAR);
  VERIFICAR(aura_sonda_cambio(&largos, b, false) == AURA_SONDA_SIN_LUGAR);

  // Espera hasta reintentar una muestra sin confirmar: 15 s, duplicando, tope 5 min
  VERIFICAR(aura_espera_confirmacion_ms(0) == 15000u);
  VERIFICAR(aura_espera_confirmacion_ms(1) == 30000u);
  VERIFICAR(aura_espera_confirmacion_ms(2) == 60000u);
  VERIFICAR(aura_espera_confirmacion_ms(4) == 240000u);
  VERIFICAR(aura_espera_confirmacion_ms(5) == 300000u);
  VERIFICAR(aura_espera_confirmacion_ms(40) == 300000u);     // sin desbordar el corrimiento
  VERIFICAR(aura_espera_confirmacion_ms(0xFFFFFFFFu) == 300000u);

  RESUMEN();
}
