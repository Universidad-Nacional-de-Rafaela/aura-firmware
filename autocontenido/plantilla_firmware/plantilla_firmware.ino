// GENERADO por herramientas/generar_autocontenidos.sh: no editar. Fuente: ejemplos/plantilla_firmware/
/*
 * AURA - plantilla de firmware
 *
 * Punto de partida para un firmware nuevo. Copiar esta carpeta a firmware/Fnnn_<descripcion>/
 * con el número que sigue en firmware/README.md, y renombrar el .ino igual que la carpeta
 * (Arduino lo exige). Ver CONTRIBUTING.md en la raíz del repo.
 *
 * Reglas del contrato de AURA (docs/CONTRATO_MQTT.md) que conviene tener presentes
 * desde la primera línea:
 *   - Solo mediciones en "values". Diagnóstico (cola, batería, RSSI) va a "status".
 *   - Una sonda que falla NO se informa con un valor centinela ni con otro sensor:
 *     su campo no se envía, y la falla va por alerts/<device_id>/sensor.
 *   - Los parámetros configurables se validan contra un rango antes de aplicarlos
 *     y se guardan en memoria no volátil.
 *   - Nada de credenciales en este archivo: van en config_local.h, que no se versiona.
 *
 * IDE: placa "XIAO_ESP32S3", USB CDC On Boot: ENABLED (si no, el monitor serie
 * no muestra nada).
 */

#if __has_include("config_local.h")
#include "config_local.h"
#endif

// Valores por defecto si no hay config_local.h. Nunca poner acá valores reales.
#ifndef INTERVALO_MUESTRA_MS
#define INTERVALO_MUESTRA_MS 60000UL
#endif

// Rango físico válido de la medición. Fuera de esto, la lectura es una falla.
const float MEDICION_MIN = -55.0;
const float MEDICION_MAX = 125.0;

unsigned long ultimaMuestra = 0;

// Devuelve la medición, o NAN si el sensor no respondió o dio un valor imposible.
// Reemplazar por la lectura del sensor real.
float leerMedicion() {
  float valor = NAN;  // TODO: leer el sensor
  if (isnan(valor) || valor < MEDICION_MIN || valor > MEDICION_MAX) return NAN;
  return valor;
}

void setup() {
  Serial.begin(115200);
  Serial.println("plantilla_firmware: reemplazar leerMedicion() y el envío");
}

void loop() {
  unsigned long ahora = millis();
  if (ahora - ultimaMuestra >= INTERVALO_MUESTRA_MS) {
    ultimaMuestra = ahora;
    float valor = leerMedicion();
    if (isnan(valor)) {
      Serial.println("[SENSOR] sin lectura válida: no se envía medición");  // y alerta
    } else {
      Serial.printf("[SENSOR] %.2f\n", valor);  // TODO: enviar por mesh o LoRaWAN
    }
  }
  delay(10);
}
