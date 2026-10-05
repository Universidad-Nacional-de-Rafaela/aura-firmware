// GENERADO por herramientas/generar_autocontenidos.sh: no editar. Fuente: ejemplos/sensor_ejemplo/
/*
 * AURA - nodo sensor de ejemplo (XIAO ESP32S3), contrato v3.0
 *
 * Muestra el uso de comun/nodo_mesh.h con lo minimo: una medicion, un
 * parametro configurable y nada de hardware extra. Con la placa pelada, lo
 * unico fisico que se puede medir es la temperatura interna del chip, y se
 * publica con ese nombre (temp_chip_c): es un dato real, no se hace pasar por
 * la temperatura de ningun otro lado. Alcanza para validar la cadena de punta
 * a punta: apretando el chip con el dedo se ve subir el valor en la base.
 *
 * No conoce AURA: se identifica por MAC, y el gateway la traduce a su
 * device_id. Las MAC van en config_local.h (ver config_local.h.example).
 *
 * set_config acepta {"intervalo_s": 5..86400}, guardado en flash.
 *
 * IDE: Placa "XIAO_ESP32S3". USB CDC On Boot: ENABLED, si no el monitor
 * serie no muestra nada. Biblioteca: ArduinoJson (ver bibliotecas.txt).
 */

#if __has_include("config_local.h")
#include "config_local.h"
#endif

#include <Preferences.h>
#include "nodo_mesh.h"

// Valores por defecto si no hay config_local.h. En cero = sin configurar: el
// nodo mide y guarda, pero no envia nada.
#ifndef MAC_PADRE
#define MAC_PADRE    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#endif
#ifndef MAC_GATEWAY
#define MAC_GATEWAY  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#endif
#ifndef MAC_ESPERADA
#define MAC_ESPERADA {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#endif

const uint8_t mac_padre[6]    = MAC_PADRE;
const uint8_t mac_gateway[6]  = MAC_GATEWAY;
const uint8_t mac_esperada[6] = MAC_ESPERADA;

// ===== Configuracion remota =====
const uint32_t INTERVALO_MIN_S = 5;
const uint32_t INTERVALO_MAX_S = 86400;
uint32_t intervalo_s = 60;

Preferences prefs;

// Valida TODO antes de aplicar nada: un comando mal armado no puede dejar el
// nodo inutilizable (contrato §3.3).
bool aplicar_config(JsonObjectConst params, String& motivo) {
  if (params.isNull() || params.size() == 0) { motivo = "sin_parametros"; return false; }
  for (JsonPairConst p : params) {
    if (strcmp(p.key().c_str(), "intervalo_s") != 0) {
      motivo = String("parametro_desconocido:") + p.key().c_str();
      return false;
    }
  }
  JsonVariantConst v = params["intervalo_s"];
  if (!v.is<uint32_t>()) { motivo = "intervalo_s_no_es_entero"; return false; }
  uint32_t nuevo = v.as<uint32_t>();
  if (nuevo < INTERVALO_MIN_S || nuevo > INTERVALO_MAX_S) { motivo = "intervalo_s_fuera_de_rango"; return false; }

  intervalo_s = nuevo;
  prefs.putUInt("intervalo_s", intervalo_s);
  Serial.printf("config aplicada: intervalo_s=%lu\n", (unsigned long)intervalo_s);
  return true;
}

void describir_config(JsonObject out) {
  out["intervalo_s"] = intervalo_s;
}

// ===== Programa =====
unsigned long ultima_muestra = 0;
bool primera = true;

void setup() {
  Serial.begin(115200);
  delay(1000);

  prefs.begin("sensor", false);
  intervalo_s = prefs.getUInt("intervalo_s", intervalo_s);
  if (intervalo_s < INTERVALO_MIN_S || intervalo_s > INTERVALO_MAX_S) intervalo_s = 60;

  NodoMeshCallbacks cb = {aplicar_config, describir_config, NULL, "red"};
  nodo_mesh_iniciar(mac_padre, mac_gateway, mac_esperada, cb);
  Serial.printf("sensor_ejemplo listo, intervalo %lu s\n", (unsigned long)intervalo_s);
}

void loop() {
  if (primera || millis() - ultima_muestra >= intervalo_s * 1000UL) {
    primera = false;
    ultima_muestra = millis();

    float temp = temperatureRead();
    // Rango del sensor interno del ESP32-S3. Fuera de esto, no es una medicion.
    bool ok = !isnan(temp) && temp > -40.0f && temp < 125.0f;
    nodo_mesh_sonda("temp_chip_c", ok, "fuera_de_rango");
    if (ok) {
      JsonDocument values;
      values["temp_chip_c"] = roundf(temp * 10.0f) / 10.0f;
      nodo_mesh_medicion(values.as<JsonObjectConst>());
    }
  }

  nodo_mesh_loop();
  delay(20);
}
