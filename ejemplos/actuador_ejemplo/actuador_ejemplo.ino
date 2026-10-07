/*
 * AURA - nodo actuador de ejemplo (XIAO ESP32S3), contrato v4.0, ESP-WIFI-MESH
 *
 * Muestra lo que sensor_ejemplo no tiene: un COMANDO propio del dispositivo,
 * ademas de set_config. Acciona el LED de la placa (GPIO 21, activo en bajo),
 * asi que no necesita hardware extra.
 *
 *   {"command": "led", "params": {"encendido": true}, "command_id": "c-1"}
 *
 * El resultado vuelve a AURA como "aplicado" o "rechazado" en
 * hw/<hw_id>/response, con la configuracion vigente. El estado del LED se
 * publica como medicion ("led_encendido": 0 o 1) cada intervalo_s y cada vez
 * que cambia: es lo que AURA grafica para saber si el actuador hizo caso.
 *
 * set_config acepta {"intervalo_s": 5..86400}, guardado en flash.
 *
 * IDE: Placa "XIAO_ESP32S3". USB CDC On Boot: ENABLED.
 * Biblioteca: ArduinoJson (ver bibliotecas.txt).
 */

#if __has_include("config_local.h")
#include "config_local.h"
#endif

#include <Preferences.h>
#include "../../comun/nodo_mesh.h"

const int PIN_LED = 21;          // LED de usuario de la XIAO ESP32S3
const bool LED_ACTIVO_BAJO = true;

// ===== Configuracion remota =====
const uint32_t INTERVALO_MIN_S = 5;
const uint32_t INTERVALO_MAX_S = 86400;
uint32_t intervalo_s = 60;

Preferences prefs;
bool led_encendido = false;
bool publicar_ya = true;   // publicar la medicion en la proxima vuelta del loop

void poner_led(bool encendido) {
  led_encendido = encendido;
  digitalWrite(PIN_LED, (encendido != LED_ACTIVO_BAJO) ? HIGH : LOW);
}

// Valida TODO antes de aplicar nada (contrato §3.3).
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
  return true;
}

void describir_config(JsonObject out) {
  out["intervalo_s"] = intervalo_s;
}

// Comandos propios. true = ejecutado. Si los parametros no validan, no se toca
// el LED y motivo explica por que: le llega a AURA como "rechazado".
bool ejecutar_comando(const char* comando, JsonObjectConst params, String& motivo) {
  if (strcmp(comando, "led") != 0) { motivo = "comando_desconocido"; return false; }
  JsonVariantConst v = params["encendido"];
  if (!v.is<bool>()) { motivo = "encendido_no_es_booleano"; return false; }
  poner_led(v.as<bool>());
  publicar_ya = true;
  Serial.printf("LED %s\n", led_encendido ? "encendido" : "apagado");
  return true;
}

unsigned long ultima_muestra = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(PIN_LED, OUTPUT);
  poner_led(false);

  prefs.begin("actuador", false);
  intervalo_s = prefs.getUInt("intervalo_s", intervalo_s);
  if (intervalo_s < INTERVALO_MIN_S || intervalo_s > INTERVALO_MAX_S) intervalo_s = 60;

  NodoMeshCallbacks cb = {aplicar_config, describir_config, ejecutar_comando, "red"};
  nodo_mesh_iniciar(cb);
  Serial.printf("actuador_ejemplo listo, intervalo %lu s\n", (unsigned long)intervalo_s);
}

void loop() {
  if (publicar_ya || millis() - ultima_muestra >= intervalo_s * 1000UL) {
    publicar_ya = false;
    ultima_muestra = millis();
    JsonDocument values;
    values["led_encendido"] = led_encendido ? 1 : 0;
    nodo_mesh_medicion(values.as<JsonObjectConst>());
  }

  nodo_mesh_loop();
  delay(20);
}
