/*
 * Banco LoRaWAN UNRaf - nodo de nivel de tanques (E2-AZ-TERR-TAN01)
 *
 * Tres sensores ultrasonicos AJ-SR04M miden la distancia al agua de 3 tanques.
 * Implementa el contrato MQTT v4.0 por LoRaWAN (hw_id = eui-<DevEUI>).
 *
 * --- CICLO DE MEDICION (fijo: ningun comando lo cambia) ---
 * Un tanque cada 20 s: tanque 1 en el segundo 0, tanque 2 en el 20 y
 * tanque 3 en el 40 de cada minuto. Al llegar al minuto vuelve al tanque 1.
 * La red (join, uplinks, reintentos) solo trabaja en los huecos entre turnos
 * y nunca ocupa un turno de medicion.
 *
 * --- MEMORIA ---
 * Las mediciones viven solo en RAM: cada tanque guarda su ultima medicion
 * hasta que la proxima la reemplaza. No se escriben en flash.
 * En flash (NVS) solo van dos cosas, y solo cuando cambian:
 *   - el buffer de nonces del join (una vez por join; evita repetir DevNonce)
 *   - intervalo_s (solo cuando llega un set_config con un valor distinto)
 * La sesion LoRaWAN NO se guarda: tras un reinicio el nodo hace un join nuevo.
 *
 * --- UPLINKS (todos confirmados) ---
 * Puerto 1, telemetria, 10 bytes, igual que antes (el codec no cambia):
 *   [0..1] contador   [2..3] dist tanque 1   [4..5] dist tanque 2
 *   [6..7] dist tanque 3   [8..9] reservado (0)
 *   dist en mm, big-endian. Sin dato: 0xFFFF sin eco, 0xFFFE demasiado cerca,
 *   0xFFFD demasiado lejos (el codec no los pone en values).
 *   Sale justo despues de medir el tanque 3, cada intervalo_s (60 s por
 *   defecto), con la ultima medicion de cada tanque guardada en RAM.
 *
 * Puerto 2, reporte bajo demanda (respuesta a un comando, NO es telemetria):
 *   [0] tipo = 0x01   [1] mascara de tanques incluidos
 *   y por cada tanque de la mascara, en orden: dist (2 bytes) + edad_s (2 bytes)
 *   edad_s = segundos desde que se midio (0xFFFF = todavia sin medir).
 *   Devuelve lo que hay en RAM: NO dispara una medicion ni mueve el ciclo.
 *
 * --- DOWNLINKS ---
 *   2 bytes: set_config intervalo_s (big-endian), 60 a 65535.
 *            Ej.: 0x01 0x2C = 300 s. Se redondea hacia arriba a un multiplo
 *            de 60 s, porque la telemetria sale despues del tanque 3.
 *   1 byte : pedido de reporte, mascara de tanques (bit0 = tanque 1,
 *            bit1 = tanque 2, bit2 = tanque 3). Ej.: 0x05 = tanques 1 y 3,
 *            0x07 = todos.
 *   Clase A: el nodo recibe el comando en la ventana que sigue a su proximo
 *   uplink (hasta 1 minuto) y responde enseguida, en un hueco entre turnos.
 */

#include <RadioLib.h>
#include <Preferences.h>
// Credenciales OTAA: van en credenciales.h (copiar credenciales.h.example), que no se versiona.
// Sin ese archivo el sketch compila (lo necesita el CI) pero no puede unirse a la red.
#if __has_include("credenciales.h")
#include "credenciales.h"
#else
static const uint64_t JOIN_EUI = 0x0000000000000000;
static const uint64_t DEV_EUI  = 0x0000000000000000;
static const uint8_t  APP_KEY[16] = {0};
#endif

// ===== PERSISTENCIA EN NVS (flash): solo datos que casi nunca cambian =====
Preferences almacen;

const char* NVS_ESPACIO   = "lorawan";
const char* NVS_NONCES    = "nonces";
// Clave nueva: la version anterior guardaba "intervalo" en milisegundos.
const char* NVS_INTERVALO = "intervalo_s";

// ===== PINES DEL WIO-SX1262 =====
#define PIN_NSS    41
#define PIN_DIO1   39
#define PIN_NRST   42
#define PIN_BUSY   40
#define PIN_ANT_SW 38

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY);

// ===== CONFIGURACION LORAWAN =====
const LoRaWANBand_t Region   = AU915;
const uint8_t       SUBBANDA = 2;

LoRaWANNode node(&radio, &Region, SUBBANDA);

// ===== DATA RATE FIJO =====
const bool    USAR_ADR = false;
const uint8_t DATARATE = 3;  // DR3 (SF9): dejan lugar para los comandos MAC

// ===== SENSORES AJ-SR04M =====
const uint8_t N_TANQUES = 3;
const uint8_t PIN_TRIG[N_TANQUES] = {D0, D2, D4};
const uint8_t PIN_ECHO[N_TANQUES] = {D1, D3, D5};

const uint8_t       N_MUESTRAS       = 5;
const uint8_t       MIN_VALIDAS      = 3;
const uint16_t      DIST_MIN_MM      = 250;
const uint16_t      DIST_MAX_MM      = 6000;
const unsigned long TIMEOUT_ECO_US   = 38000UL;
const unsigned long PAUSA_MUESTRA_MS = 60UL;

const uint16_t SIN_ECO         = 0xFFFF;
const uint16_t DEMASIADO_CERCA = 0xFFFE;
const uint16_t DEMASIADO_LEJOS = 0xFFFD;

const uint16_t RESERVADO = 0x0000;

const size_t TAM_PAYLOAD = 10;
static_assert(TAM_PAYLOAD == 2 + 2 * N_TANQUES + 2,
              "TAM_PAYLOAD = contador + distancias + reservado");

// ===== CICLO DE MEDICION =====
const unsigned long PASO_MEDICION_MS = 20000UL;                    // un tanque cada 20 s
const unsigned long CICLO_MS         = PASO_MEDICION_MS * N_TANQUES;  // 60 s

// Ultima medicion de cada tanque. SOLO EN RAM: la proxima medicion la pisa.
struct Medicion {
  uint16_t      dist;      // mm, o SIN_ECO / DEMASIADO_CERCA / DEMASIADO_LEJOS
  unsigned long instante;  // millis() en que se midio
  bool          hecha;     // false hasta la primera medicion de ese tanque
};
Medicion ultima[N_TANQUES];

unsigned long proximaMedicion = 0;  // millis() del proximo turno
uint8_t       turno           = 0;  // tanque que toca medir (0 = tanque 1)

// ===== TELEMETRIA Y REPORTES =====
const uint8_t PUERTO_TELEMETRIA       = 1;
const uint8_t PUERTO_REPORTE          = 2;
const uint8_t TIPO_REPORTE_MEDICIONES = 0x01;
const uint8_t MASCARA_TODOS           = (1 << N_TANQUES) - 1;

const uint16_t INTERVALO_MIN_S = CICLO_MS / 1000UL;  // 60: cada tanque se mide una vez por minuto
uint16_t       intervaloEnvioS = INTERVALO_MIN_S;    // intervalo_s (set_config)
uint16_t       ciclosDesdeEnvio = 0xFFFF;            // arranca alto: el 1er ciclo ya envia
uint16_t       contador = 0;

struct EnvioPendiente {
  bool          activo;
  uint8_t       payload[TAM_PAYLOAD];
  uint16_t      contador;
  uint16_t      intentos;
  unsigned long proximoIntento;
};
EnvioPendiente telemetria = {};

uint8_t       reporteMascara        = 0;  // tanques pedidos; 0 = no hay reporte pendiente
uint8_t       reporteIntentos       = 0;
unsigned long reporteProximoIntento = 0;

// ===== REINTENTOS Y JOIN =====
const bool          CONFIRMADO            = true;
const unsigned long ESPERA_BASE           = 2000UL;
const unsigned long ESPERA_MAX            = 60000UL;
const unsigned long ESPERA_AZAR           = 1000UL;
const unsigned long DURACION_INTENTO_MS   = 6000UL;   // lo que puede tardar un uplink con sus ventanas RX
const unsigned long MARGEN_UNION_MS       = 9000UL;   // lo que puede tardar un join
const uint8_t       MAX_INTENTOS_REPORTE  = 4;
const unsigned long ESPERA_UNION_BASE_MS  = 30000UL;
const unsigned long ESPERA_UNION_MAX_MS   = 300000UL;

bool          necesitaUnirse = true;
unsigned long proximoUnion   = 0;
uint8_t       fallosUnion    = 0;

// ===== PROTOTIPOS =====
void iniciarSensores();
uint16_t leerDistanciaMm(uint8_t i);
void imprimirDistancia(uint8_t i, uint16_t d);
void medirTurno();
void cerrarCiclo();
uint16_t ciclosPorEnvio();
void armarTelemetria();
void atenderRed();
void intentarTelemetria();
void intentarReporte();
bool enviarUplink(uint8_t puerto, const uint8_t* payload, size_t tam);
void procesarBajada(const uint8_t* datos, size_t tam);
unsigned long calcularEspera(uint16_t intento);
void unirse();
void fijarDatarate();
void restaurarNonces();
void cargarIntervalo();
void guardarBuffer(const char* clave, const uint8_t* datos, size_t tam);
long msHasta(unsigned long instante);
void detener();

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) { }

  Serial.println();
  Serial.println(F("=== Banco LoRaWAN UNRaf - nodo de tanques ==="));

  iniciarSensores();

  pinMode(PIN_ANT_SW, OUTPUT);
  digitalWrite(PIN_ANT_SW, HIGH);

  Serial.print(F("Iniciando radio SX1262... "));
  int estado = radio.begin();
  if (estado != RADIOLIB_ERR_NONE) {
    Serial.print(F("FALLO, codigo "));
    Serial.println(estado);
    detener();
  }
  Serial.println(F("OK"));

  radio.setDio2AsRfSwitch(true);

  // LoRaWAN 1.0.x: sin NWK_KEY (puntero nulo), ver credenciales.
  node.beginOTAA(JOIN_EUI, DEV_EUI, nullptr, APP_KEY);

  almacen.begin(NVS_ESPACIO, false);
  restaurarNonces();
  cargarIntervalo();

  // El ciclo de medicion arranca ya: no espera al join. El join se intenta
  // en los huecos entre turnos (ver atenderRed).
  proximaMedicion = millis() + 1000UL;
  proximoUnion    = millis();
  Serial.println(F("Ciclo: tanque 1 en s0, tanque 2 en s20, tanque 3 en s40"));
  Serial.println();
}

// ===== LOOP PRINCIPAL =====
void loop() {
  if (msHasta(proximaMedicion) <= 0) {
    medirTurno();      // la medicion siempre va primero
  } else {
    atenderRed();      // la red solo usa los huecos entre turnos
  }
  delay(10);
}

// ===== MEDICION =====
void iniciarSensores() {
  for (uint8_t i = 0; i < N_TANQUES; i++) {
    pinMode(PIN_TRIG[i], OUTPUT);
    digitalWrite(PIN_TRIG[i], LOW);
    pinMode(PIN_ECHO[i], INPUT);
    ultima[i].dist     = SIN_ECO;
    ultima[i].instante = 0;
    ultima[i].hecha    = false;
  }
}

// Mide el tanque que toca y avanza al siguiente turno.
void medirTurno() {
  // Si algo retraso el nodo uno o mas turnos completos, los salta en vez de
  // medir varios tanques seguidos: cada tanque conserva su segundo del minuto.
  long atraso = -msHasta(proximaMedicion);
  if (atraso >= (long)PASO_MEDICION_MS) {
    uint32_t perdidos = (uint32_t)atraso / PASO_MEDICION_MS;
    proximaMedicion += perdidos * PASO_MEDICION_MS;
    turno = (turno + perdidos) % N_TANQUES;
    Serial.print(F("[MED] turnos salteados: "));
    Serial.println(perdidos);
  }

  uint8_t t = turno;
  unsigned long inicio = millis();
  uint16_t d = leerDistanciaMm(t);

  ultima[t].dist     = d;       // RAM: pisa la medicion anterior de este tanque
  ultima[t].instante = inicio;
  ultima[t].hecha    = true;
  imprimirDistancia(t, d);

  proximaMedicion += PASO_MEDICION_MS;   // se suma al turno anterior, no a millis(): sin deriva
  turno = (turno + 1) % N_TANQUES;

  if (t == N_TANQUES - 1) {
    cerrarCiclo();
  }
}

uint16_t leerDistanciaMm(uint8_t i) {
  uint16_t validas[N_MUESTRAS];
  uint8_t nValidas = 0, nCerca = 0, nLejos = 0;

  for (uint8_t k = 0; k < N_MUESTRAS; k++) {
    digitalWrite(PIN_TRIG[i], LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_TRIG[i], HIGH);
    delayMicroseconds(15);
    digitalWrite(PIN_TRIG[i], LOW);

    unsigned long us = pulseIn(PIN_ECHO[i], HIGH, TIMEOUT_ECO_US);
    if (us > 0) {
      uint32_t mm = (uint32_t)(us * 0.343f / 2.0f);
      if (mm < DIST_MIN_MM)      nCerca++;
      else if (mm > DIST_MAX_MM) nLejos++;
      else                       validas[nValidas++] = (uint16_t)mm;
    }
    delay(PAUSA_MUESTRA_MS);
  }

  if (nValidas >= MIN_VALIDAS) {
    for (uint8_t a = 1; a < nValidas; a++) {
      uint16_t v = validas[a];
      int8_t b = a - 1;
      while (b >= 0 && validas[b] > v) { validas[b + 1] = validas[b]; b--; }
      validas[b + 1] = v;
    }
    return validas[nValidas / 2];   // mediana
  }
  if (nCerca >= MIN_VALIDAS) return DEMASIADO_CERCA;
  if (nLejos >= MIN_VALIDAS) return DEMASIADO_LEJOS;
  return SIN_ECO;
}

void imprimirDistancia(uint8_t i, uint16_t d) {
  Serial.print(F("[MED] Tanque "));
  Serial.print(i + 1);
  Serial.print(F(": "));
  if (d == SIN_ECO)              Serial.println(F("sin eco"));
  else if (d == DEMASIADO_CERCA) Serial.println(F("demasiado cerca (zona ciega)"));
  else if (d == DEMASIADO_LEJOS) Serial.println(F("demasiado lejos"));
  else { Serial.print(d); Serial.println(F(" mm")); }
}

// ===== TELEMETRIA =====
uint16_t ciclosPorEnvio() {
  return ((uint32_t)intervaloEnvioS + (INTERVALO_MIN_S - 1)) / INTERVALO_MIN_S;
}

// Se llama al terminar el turno del tanque 3: cierra el minuto.
void cerrarCiclo() {
  if (ciclosDesdeEnvio < 0xFFFF) ciclosDesdeEnvio++;
  if (ciclosDesdeEnvio >= ciclosPorEnvio()) {
    armarTelemetria();
    ciclosDesdeEnvio = 0;
  }
}

// Toma una foto de lo que hay en RAM y la deja lista para enviar.
void armarTelemetria() {
  if (telemetria.activo) {
    Serial.print(F("[TX] PERDIDO contador="));
    Serial.print(telemetria.contador);
    Serial.println(F(": no se pudo entregar antes de la proxima telemetria"));
  }

  telemetria.contador = contador++;
  telemetria.payload[0] = (telemetria.contador >> 8) & 0xFF;
  telemetria.payload[1] = telemetria.contador & 0xFF;
  for (uint8_t i = 0; i < N_TANQUES; i++) {
    uint16_t d = ultima[i].hecha ? ultima[i].dist : SIN_ECO;
    telemetria.payload[2 + 2 * i] = (d >> 8) & 0xFF;
    telemetria.payload[3 + 2 * i] = d & 0xFF;
  }
  telemetria.payload[8] = (RESERVADO >> 8) & 0xFF;
  telemetria.payload[9] = RESERVADO & 0xFF;

  telemetria.intentos       = 0;
  telemetria.proximoIntento = millis();
  telemetria.activo         = true;
}

// ===== RED: solo en los huecos entre turnos =====
void atenderRed() {
  long libre = msHasta(proximaMedicion);   // ms que faltan para el proximo turno

  if (necesitaUnirse) {
    if (libre >= (long)MARGEN_UNION_MS && msHasta(proximoUnion) <= 0) {
      unirse();
    }
    return;
  }

  // No arrancar un envio que pueda pisar el proximo turno de medicion.
  if (libre < (long)DURACION_INTENTO_MS) return;
  if (node.timeUntilUplink() > 0) return;   // duty cycle

  if (telemetria.activo && msHasta(telemetria.proximoIntento) <= 0) {
    intentarTelemetria();
  } else if (reporteMascara != 0 && msHasta(reporteProximoIntento) <= 0) {
    intentarReporte();
  }
}

void intentarTelemetria() {
  telemetria.intentos++;
  Serial.print(F("[TX] telemetria contador="));
  Serial.print(telemetria.contador);
  Serial.print(F(" intento "));
  Serial.print(telemetria.intentos);
  Serial.print(F(" ... "));

  if (enviarUplink(PUERTO_TELEMETRIA, telemetria.payload, TAM_PAYLOAD)) {
    telemetria.activo = false;
    return;
  }

  unsigned long espera = calcularEspera(telemetria.intentos + 1);
  telemetria.proximoIntento = millis() + espera;
  Serial.print(F("     reintento en "));
  Serial.print(espera / 1000.0, 1);
  Serial.println(F(" s (si hay hueco antes del proximo turno)"));
}

// Responde un pedido de reporte con lo que hay en RAM. No mide nada.
void intentarReporte() {
  uint8_t payload[2 + 4 * N_TANQUES];
  size_t n = 0;
  unsigned long ahora = millis();

  payload[n++] = TIPO_REPORTE_MEDICIONES;
  payload[n++] = reporteMascara;
  for (uint8_t i = 0; i < N_TANQUES; i++) {
    if (!(reporteMascara & (1 << i))) continue;

    uint16_t d    = SIN_ECO;
    uint16_t edad = 0xFFFF;                     // todavia sin medir
    if (ultima[i].hecha) {
      d = ultima[i].dist;
      unsigned long s = (ahora - ultima[i].instante) / 1000UL;
      edad = (s > 0xFFFE) ? 0xFFFE : (uint16_t)s;
    }
    payload[n++] = (d >> 8) & 0xFF;
    payload[n++] = d & 0xFF;
    payload[n++] = (edad >> 8) & 0xFF;
    payload[n++] = edad & 0xFF;
  }

  reporteIntentos++;
  Serial.print(F("[TX] reporte mascara=0x0"));
  Serial.print(reporteMascara, HEX);
  Serial.print(F(" intento "));
  Serial.print(reporteIntentos);
  Serial.print(F(" ... "));

  if (enviarUplink(PUERTO_REPORTE, payload, n)) {
    reporteMascara  = 0;
    reporteIntentos = 0;
    return;
  }

  if (reporteIntentos >= MAX_INTENTOS_REPORTE) {
    Serial.println(F("[TX] reporte descartado: sin ACK tras varios intentos"));
    reporteMascara  = 0;
    reporteIntentos = 0;
    return;
  }
  reporteProximoIntento = millis() + calcularEspera(reporteIntentos + 1);
}

// Un uplink confirmado. Devuelve true si el servidor lo confirmo.
// Si el servidor aprovecha la ventana para mandar un comando, lo procesa.
bool enviarUplink(uint8_t puerto, const uint8_t* payload, size_t tam) {
  LoRaWANEvent_t bajada = {};
  uint8_t rxBuffer[255];
  size_t  rxTam = 0;

  int estado = node.sendReceive(payload, tam, puerto, rxBuffer, &rxTam,
                                CONFIRMADO, nullptr, &bajada);

  if (estado == RADIOLIB_ERR_NETWORK_NOT_JOINED) {
    Serial.println(F("sin sesion"));
    necesitaUnirse = true;
    proximoUnion   = millis();
    return false;
  }

  if (estado < RADIOLIB_ERR_NONE) {
    Serial.print(F("ERROR, codigo "));
    Serial.println(estado);
    return false;
  }

  if (CONFIRMADO && !bajada.confirming) {
    Serial.println(F("sin ACK del servidor"));
    return false;
  }

  if (estado > 0) {
    Serial.print(F("confirmado, ACK en RX"));
    Serial.println(estado);
    if (rxTam > 0) {
      procesarBajada(rxBuffer, rxTam);
    }
  } else {
    Serial.println(F("enviado (sin downlink)"));
  }
  return true;
}

// ===== COMANDOS POR DOWNLINK =====
void procesarBajada(const uint8_t* datos, size_t tam) {
  if (tam == 2) {
    // set_config: intervalo_s, big-endian.
    uint16_t nuevoS = ((uint16_t)datos[0] << 8) | datos[1];
    if (nuevoS < INTERVALO_MIN_S) {
      Serial.print(F(">>> set_config RECHAZADO: intervalo_s="));
      Serial.print(nuevoS);
      Serial.print(F(" es menor al minimo ("));
      Serial.print(INTERVALO_MIN_S);
      Serial.println(F(" s) <<<"));
      return;
    }
    intervaloEnvioS = nuevoS;
    // Flash: solo si el valor cambio de verdad.
    if (almacen.getUInt(NVS_INTERVALO, 0) != nuevoS) {
      almacen.putUInt(NVS_INTERVALO, nuevoS);
    }
    Serial.print(F(">>> set_config: intervalo_s="));
    Serial.print(nuevoS);
    Serial.print(F(" (la telemetria sale cada "));
    Serial.print(ciclosPorEnvio() * INTERVALO_MIN_S);
    Serial.println(F(" s) <<<"));

  } else if (tam == 1) {
    // Pedido de reporte: mascara de tanques.
    uint8_t mascara = datos[0] & MASCARA_TODOS;
    if (mascara == 0) {
      Serial.println(F(">>> reporte IGNORADO: la mascara no incluye ningun tanque <<<"));
      return;
    }
    if (reporteMascara == 0) {
      reporteIntentos       = 0;
      reporteProximoIntento = millis();
    }
    reporteMascara |= mascara;
    Serial.print(F(">>> reporte pedido, mascara=0x0"));
    Serial.print(reporteMascara, HEX);
    Serial.println(F(" (se responde en el proximo hueco, sin medir) <<<"));

  } else {
    Serial.print(F(">>> comando desconocido de "));
    Serial.print((unsigned)tam);
    Serial.println(F(" bytes, ignorado <<<"));
  }
}

unsigned long calcularEspera(uint16_t intento) {
  unsigned long espera = ESPERA_MAX;
  if (intento >= 2 && intento - 2 < 5) {
    espera = ESPERA_BASE << (intento - 2);
    if (espera > ESPERA_MAX) {
      espera = ESPERA_MAX;
    }
  }
  espera += random(ESPERA_AZAR);

  unsigned long minimo = node.timeUntilUplink();
  if (espera < minimo) {
    espera = minimo;
  }
  return espera;
}

// ===== JOIN =====
void unirse() {
  Serial.print(F("Uniendo a la red (OTAA)... "));
  int estado = node.activateOTAA();

  // Unico dato de LoRaWAN que se guarda en flash: el DevNonce, una vez por join.
  guardarBuffer(NVS_NONCES, node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);

  if (estado == RADIOLIB_LORAWAN_NEW_SESSION || estado == RADIOLIB_LORAWAN_SESSION_RESTORED) {
    Serial.println(F("OK - join nuevo"));
    necesitaUnirse = false;
    fallosUnion    = 0;
    fijarDatarate();
    return;
  }

  unsigned long espera = ESPERA_UNION_BASE_MS << (fallosUnion < 4 ? fallosUnion : 4);
  if (espera > ESPERA_UNION_MAX_MS) espera = ESPERA_UNION_MAX_MS;
  if (fallosUnion < 255) fallosUnion++;
  proximoUnion = millis() + espera;

  Serial.print(F("FALLO, codigo "));
  Serial.println(estado);
  Serial.println(F("Si el JoinRequest aparece en ChirpStack pero no vuelve el Accept:"));
  Serial.println(F("  - claves de credenciales.h distintas a las del device, o"));
  Serial.println(F("  - DevNonce repetido (correr scripts/reset-nonces.sh)."));
  Serial.print(F("Se reintenta en "));
  Serial.print(espera / 1000UL);
  Serial.println(F(" s; mientras tanto se sigue midiendo."));
}

void fijarDatarate() {
  node.setADR(USAR_ADR);
  if (USAR_ADR) {
    Serial.println(F("Data rate: ADR (lo elige ChirpStack)"));
    return;
  }

  int16_t estado = node.setDatarate(DATARATE);
  Serial.print(F("Data rate: DR"));
  Serial.print(DATARATE);
  if (estado == RADIOLIB_ERR_NONE) {
    Serial.println(F(" fijo, ADR apagado"));
  } else {
    Serial.print(F(" rechazado, codigo "));
    Serial.println(estado);
  }
}

// ===== NVS =====
void restaurarNonces() {
  if (!almacen.isKey(NVS_NONCES)) return;

  uint8_t buffer[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
  if (almacen.getBytes(NVS_NONCES, buffer, sizeof(buffer)) != sizeof(buffer)) return;
  node.setBufferNonces(buffer);
}

void cargarIntervalo() {
  if (!almacen.isKey(NVS_INTERVALO)) return;

  uint32_t s = almacen.getUInt(NVS_INTERVALO, INTERVALO_MIN_S);
  if (s >= INTERVALO_MIN_S && s <= 65535UL) {
    intervaloEnvioS = (uint16_t)s;
    Serial.print(F("intervalo_s recuperado de NVS: "));
    Serial.println(intervaloEnvioS);
  }
}

void guardarBuffer(const char* clave, const uint8_t* datos, size_t tam) {
  almacen.putBytes(clave, datos, tam);
}

// ===== UTILIDADES =====
long msHasta(unsigned long instante) {
  return (long)(instante - millis());
}

void detener() {
  Serial.println(F("Nodo detenido. Corregir y volver a flashear."));
  while (true) {
    delay(1000);
  }
}
