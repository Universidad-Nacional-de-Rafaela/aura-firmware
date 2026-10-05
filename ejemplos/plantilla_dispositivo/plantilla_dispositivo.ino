/*
 * AURA - plantilla de dispositivo
 *
 * Punto de partida para un dispositivo nuevo. Copiar esta carpeta a dispositivos/<CÓDIGO>/
 * (el código se arma con el mapa de dispositivos/README.md) y renombrar el .ino igual que la
 * carpeta, por ejemplo E1-PB-LECA-HFR01.ino (Arduino lo exige). Ver CONTRIBUTING.md.
 *
 * Viene armada para la mesh ESP-NOW (interior) con comun/nodo_mesh.h, que se ocupa de la
 * cola en flash, el ingest_id, los reintentos hasta que AURA confirma, set_config y las
 * alertas. Lo que completa el grupo está marcado con TODO. Para LoRaWAN (exterior), ver
 * IC-lorawan-test.
 *
 * Reglas del contrato de AURA (docs/CONTRATO_MQTT.md) que conviene tener presentes
 * desde la primera línea:
 *   - Solo mediciones en "values". Diagnóstico (cola, batería, RSSI) va a "status":
 *     de eso se ocupa nodo_mesh.h con el reporte.
 *   - Una sonda que falla NO se informa con un valor centinela ni con otro sensor:
 *     su campo no se envía, y la falla va como alerta (nodo_mesh_sonda()).
 *   - Los parámetros configurables se validan contra un rango antes de aplicarlos
 *     y se guardan en memoria no volátil (aplicar_config()).
 *   - Nada de credenciales ni MAC en este archivo: van en config_local.h, que no se versiona.
 *
 * IDE: placa "XIAO_ESP32S3", USB CDC On Boot: ENABLED (si no, el monitor serie
 * no muestra nada). Bibliotecas: las de bibliotecas.txt.
 */

#if __has_include("config_local.h")
#include "config_local.h"
#endif

#include <Preferences.h>
#include "../../comun/nodo_mesh.h"

// Valores por defecto si no hay config_local.h. Nunca poner acá valores reales.
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

// Nombre del campo en "values", con unidad (contrato §3.1). TODO: el de tu medición.
const char* CAMPO = "temp_c";

// Rango físico válido de la medición. Fuera de esto, la lectura es una falla.
const float MEDICION_MIN = -55.0;
const float MEDICION_MAX = 125.0;

// Parámetros configurables por set_config, con su rango (contrato §3.3).
const uint32_t INTERVALO_MIN_S = 5;
const uint32_t INTERVALO_MAX_S = 86400;
uint32_t intervalo_s = 60;

Preferences prefs;

// Devuelve la medición, o NAN si el sensor no respondió o dio un valor imposible.
float leerMedicion() {
  float valor = NAN;  // TODO: leer el sensor
  if (isnan(valor) || valor < MEDICION_MIN || valor > MEDICION_MAX) return NAN;
  return valor;
}

// Valida TODOS los parámetros antes de aplicar alguno. Si uno no valida, no se toca nada
// y motivo explica por qué: eso le llega a AURA como "rechazado".
bool aplicar_config(JsonObjectConst params, String& motivo) {
  if (params.isNull() || params.size() == 0) { motivo = "sin_parametros"; return false; }
  for (JsonPairConst p : params) {
    if (strcmp(p.key().c_str(), "intervalo_s") != 0) {  // TODO: tus parámetros
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

// La configuración vigente. "intervalo_s" no puede faltar: con él AURA sabe cuándo
// el nodo dejó de transmitir.
void describir_config(JsonObject out) {
  out["intervalo_s"] = intervalo_s;
}

unsigned long ultimaMuestra = 0;
bool primera = true;

void setup() {
  Serial.begin(115200);
  delay(1000);

  prefs.begin("dispositivo", false);
  intervalo_s = prefs.getUInt("intervalo_s", intervalo_s);
  if (intervalo_s < INTERVALO_MIN_S || intervalo_s > INTERVALO_MAX_S) intervalo_s = 60;

  // TODO: inicializar el sensor.

  // Alimentación: "red", "bateria" o "desconocida". Si cambia en marcha,
  // llamar a nodo_mesh_alimentacion("bateria").
  NodoMeshCallbacks cb = {aplicar_config, describir_config, NULL, "red"};
  nodo_mesh_iniciar(mac_padre, mac_gateway, mac_esperada, cb);
}

void loop() {
  if (primera || millis() - ultimaMuestra >= intervalo_s * 1000UL) {
    primera = false;
    ultimaMuestra = millis();

    float valor = leerMedicion();
    nodo_mesh_sonda(CAMPO, !isnan(valor));   // alerta solo cuando cambia
    if (!isnan(valor)) {
      JsonDocument values;
      values[CAMPO] = valor;                 // varias sondas = varios campos acá
      nodo_mesh_medicion(values.as<JsonObjectConst>());
    } else {
      Serial.println("[SENSOR] sin lectura válida: no se envía medición");
    }
  }

  nodo_mesh_loop();
  delay(20);
}
