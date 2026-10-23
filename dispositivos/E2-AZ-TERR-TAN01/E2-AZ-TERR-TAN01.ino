/*
 * E2-AZ-TERR-TAN01 - nivel de 3 tanques de agua, terraza del Edificio 2 (LoRaWAN, clase A)
 *
 * Tres sensores ultrasonicos AJ-SR04M miden la distancia al agua de 3 tanques.
 * Implementa el contrato MQTT v4.0 por LoRaWAN (hw_id = eui-<DevEUI>). El formato de los
 * bytes es el mismo esquema que usa el codec de E1-AZ-TERR-TAN01 (lo mantiene la catedra):
 * medicion en el fPort 1, reporte de comandos en el fPort 2 y comandos con "ref" en el fPort 10.
 * El par de este sketch es codec.js, en esta carpeta: si cambia el formato de uno, cambia el otro.
 *
 * --- CICLO DE MEDICION (intervalo_s, configurable con set_config) ---
 * Cada intervalo_s se miden los 3 tanques, repartidos en turnos iguales de intervalo_s / 3:
 * con 60 s (por defecto), el tanque 1 en el segundo 0, el 2 en el 20 y el 3 en el 40; con
 * 300 s, cada 100 s. Despues del tanque 3 sale la medicion por LoRaWAN. Si set_config cambia
 * intervalo_s, los turnos se recalculan solos y el ciclo arranca de nuevo por el tanque 1.
 * La red (join, uplinks, reintentos) y medir_ahora solo usan los huecos entre turnos.
 *
 * --- MEMORIA ---
 * Las mediciones viven solo en RAM: cada tanque guarda su ultima medicion hasta que la proxima
 * la reemplaza. En flash (NVS) solo van dos cosas, y solo cuando cambian:
 *   - el buffer de nonces del join (una vez por join; evita repetir DevNonce)
 *   - intervalo_s (solo cuando un set_config lo cambia)
 * La sesion LoRaWAN NO se guarda: tras un reinicio el nodo hace un join nuevo.
 *
 * --- SUBIDA (uplinks confirmados) ---
 * fPort 1, medicion, 6 bytes, big-endian:
 *   [0..1] distancia tanque 1 en mm (= cm x 10)   [2..3] tanque 2   [4..5] tanque 3
 *   El codec la pasa a cm con un decimal (2462 -> 246.2 cm).
 *   Sin medicion valida: 0xFFFF sin eco, 0xFFFD demasiado lejos (el codec no los pone en
 *   values, contrato 3.1). 0xFFFE = demasiado cerca: el agua esta dentro de la zona ciega del
 *   sensor, o sea el tanque esta lleno. No es una falla: el codec lo informa como 25 cm, el
 *   nivel maximo que el sensor puede medir (saturacion del rango).
 *   Sale al terminar cada ciclo (despues del tanque 3), con la ultima medicion de cada
 *   tanque; y ademas despues de un medir_ahora, con una medicion nueva de los tres.
 * fPort 2, reporte de un comando (contrato 3.4), 4 bytes:
 *   [0] ref del comando   [1] resultado: 0 aplicado, 1 fuera de rango, 2 comando desconocido
 *   [2..3] intervalo_s vigente
 *
 * --- BAJADA (fPort 10; el lorawan-bridge pone el ref) ---
 *   01 ref         medir_ahora: mide los 3 tanques en el proximo hueco entre turnos, sin
 *                  mover el ciclo, y manda la medicion por el fPort 1.
 *   02 ref hi lo   set_config intervalo_s (60 a 43200): cada cuanto se miden los 3 tanques
 *                  y se envia. Los turnos pasan a intervalo_s / 3 y el ciclo arranca de nuevo.
 *   Cada comando se contesta con un reporte en el fPort 2. Un comando desconocido o mal
 *   armado se contesta con resultado 2 y no cambia nada.
 *   Clase A: el nodo recibe el comando en la ventana que sigue a su proximo uplink (hasta
 *   intervalo_s despues de encolarlo) y lo atiende en el hueco siguiente.
 */

#include <RadioLib.h>
#include <Preferences.h>
// Credenciales OTAA (JOIN_EUI, DEV_EUI, APP_KEY): van en config_local.h, que esta en el
// .gitignore y NUNCA se versiona (el repo es publico). Hay un molde en config_local.h.example.
// Sin config_local.h el sketch compila (el CI lo necesita) pero no puede unirse a la red.
#if __has_include("config_local.h")
#include "config_local.h"
#else
static const uint64_t JOIN_EUI = 0x0000000000000000;
static const uint64_t DEV_EUI  = 0x0000000000000000;
static const uint8_t  APP_KEY[16] = {0};
#endif

// ===== PERSISTENCIA EN NVS (flash): solo datos que casi nunca cambian =====
Preferences almacen;

const char* NVS_ESPACIO   = "lorawan";
const char* NVS_NONCES    = "nonces";
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
const uint8_t DATARATE = 3;  // DR3 (SF9): deja lugar para los comandos MAC

// ===== SENSORES AJ-SR04M =====
const uint8_t N_TANQUES = 3;
const uint8_t PIN_TRIG[N_TANQUES] = {D0, D2, D4};
const uint8_t PIN_ECHO[N_TANQUES] = {D1, D3, D5};

const uint8_t       N_MUESTRAS       = 5;
const uint8_t       MIN_VALIDAS      = 3;
// Con el agua en la zona ciega el AJ-SR04M devuelve ecos cortos mezclados con lecturas sin eco:
// alcanzan 2 de 5 "demasiado cerca" para decir que el tanque esta lleno y no que el sensor falla.
const uint8_t       MIN_CERCA        = 2;
const uint16_t      DIST_MIN_MM      = 250;
const uint16_t      DIST_MAX_MM      = 6000;
const unsigned long TIMEOUT_ECO_US   = 38000UL;
const unsigned long PAUSA_MUESTRA_MS = 60UL;

const uint16_t SIN_ECO         = 0xFFFF;
const uint16_t DEMASIADO_CERCA = 0xFFFE;
const uint16_t DEMASIADO_LEJOS = 0xFFFD;

// ===== CICLO DE MEDICION =====
const unsigned long ARRANQUE_CICLO_MS = 1000UL;   // primer turno, despues de arrancar o de un set_config
// Lo que puede tardar medir los 3 tanques (5 muestras de hasta ~100 ms cada una), con margen.
const unsigned long DURACION_MEDIR_TODOS_MS = 3000UL;

// Ultima medicion de cada tanque. SOLO EN RAM: la proxima medicion la pisa.
struct Medicion {
  uint16_t      dist;      // mm, o SIN_ECO / DEMASIADO_CERCA / DEMASIADO_LEJOS
  unsigned long instante;  // millis() en que se midio
  bool          hecha;     // false hasta la primera medicion de ese tanque
};
Medicion ultima[N_TANQUES];

unsigned long inicioCiclo     = 0;      // millis() del turno del tanque 1 de este ciclo
unsigned long proximaMedicion = 0;      // millis() del proximo turno
uint8_t       turno           = 0;      // tanque que toca medir (0 = tanque 1)
bool          medicionPedida  = false;  // llego un medir_ahora y todavia no se midio

// ===== PUERTOS Y COMANDOS (iguales a codec.js) =====
const uint8_t PUERTO_MEDICION = 1;
const uint8_t PUERTO_REPORTE  = 2;
const uint8_t PUERTO_COMANDOS = 10;   // CS_FPORT_COMANDOS del lorawan-bridge

const uint8_t CMD_MEDIR_AHORA = 0x01;  // 01 ref
const uint8_t CMD_SET_CONFIG  = 0x02;  // 02 ref hi lo

const uint8_t RES_APLICADO            = 0;
const uint8_t RES_FUERA_DE_RANGO      = 1;
const uint8_t RES_COMANDO_DESCONOCIDO = 2;

const size_t TAM_MEDICION = 2 * N_TANQUES;  // 6 bytes
const size_t TAM_REPORTE  = 4;

// ===== INTERVALO DE MEDICION Y ENVIO (set_config) =====
// Con 60 s los turnos quedan a 20 s: deja lugar entre turnos para un uplink con sus
// reintentos y para el join.
const uint16_t INTERVALO_MIN_S = 60;
const uint16_t INTERVALO_MAX_S = 43200;               // 12 h, igual que el codec
uint16_t       intervaloS      = INTERVALO_MIN_S;     // duracion del ciclo de los 3 tanques

// ===== ENVIOS PENDIENTES =====
struct EnvioPendiente {
  bool          activo;
  uint8_t       payload[TAM_MEDICION];
  uint16_t      intentos;
  unsigned long proximoIntento;
};
EnvioPendiente medicion = {};

// Reportes de comandos (contrato 3.4). Una cola chica: en clase A llega a lo sumo un
// comando por uplink, asi que casi nunca hay mas de uno esperando.
struct Reporte {
  uint8_t  ref;
  uint8_t  resultado;
  uint16_t intervaloS;
};
const uint8_t MAX_REPORTES = 4;
Reporte       reportes[MAX_REPORTES];
uint8_t       nReportes             = 0;
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
void guardarMedicion(uint8_t t, uint16_t d, unsigned long instante);
void medirTurno();
void medirTodos();
void avanzarTurno();
unsigned long instanteTurno(uint8_t t);
void reiniciarCiclo();
void armarMedicion();
void atenderRed();
void intentarMedicion();
void intentarReporte();
bool enviarUplink(uint8_t puerto, const uint8_t* payload, size_t tam);
void procesarBajada(uint8_t puerto, const uint8_t* datos, size_t tam);
void encolarReporte(uint8_t ref, uint8_t resultado);
unsigned long calcularEspera(uint16_t intento);
void unirse();
void fijarDatarate();
void restaurarNonces();
void cargarIntervalo();
long msHasta(unsigned long instante);
void detener();

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) { }

  Serial.println();
  Serial.println(F("=== E2-AZ-TERR-TAN01 - nivel de 3 tanques (LoRaWAN) ==="));

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

  // LoRaWAN 1.0.x: sin NwkKey (puntero nulo).
  node.beginOTAA(JOIN_EUI, DEV_EUI, nullptr, APP_KEY);

  almacen.begin(NVS_ESPACIO, false);
  restaurarNonces();
  cargarIntervalo();

  // El ciclo de medicion arranca ya: no espera al join. El join se intenta
  // en los huecos entre turnos (ver atenderRed).
  reiniciarCiclo();
  proximoUnion = millis();
  Serial.println();
}

// ===== LOOP PRINCIPAL =====
void loop() {
  if (msHasta(proximaMedicion) <= 0) {
    medirTurno();      // la medicion siempre va primero
  } else {
    atenderRed();      // lo demas solo usa los huecos entre turnos
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

void guardarMedicion(uint8_t t, uint16_t d, unsigned long instante) {
  ultima[t].dist     = d;       // RAM: pisa la medicion anterior de este tanque
  ultima[t].instante = instante;
  ultima[t].hecha    = true;
  imprimirDistancia(t, d);
}

// Instante del turno t (0 = tanque 1) del ciclo actual. Se calcula desde el inicio del ciclo
// y no sumando pasos, asi que no acumula deriva aunque intervalo_s no sea multiplo de 3.
unsigned long instanteTurno(uint8_t t) {
  return inicioCiclo + (uint32_t)intervaloS * 1000UL * t / N_TANQUES;
}

// Arranca un ciclo nuevo por el tanque 1. Al encender y cuando set_config cambia intervalo_s:
// asi los turnos quedan repartidos con el intervalo nuevo desde el primero.
void reiniciarCiclo() {
  inicioCiclo     = millis() + ARRANQUE_CICLO_MS;
  turno           = 0;
  proximaMedicion = inicioCiclo;
  Serial.print(F("Ciclo: los 3 tanques cada "));
  Serial.print(intervaloS);
  Serial.print(F(" s, un turno cada "));
  Serial.print(intervaloS / (float)N_TANQUES, 1);
  Serial.println(F(" s"));
}

// Pasa al turno siguiente. Al terminar el tanque 3 cierra el ciclo: deja lista la medicion
// para enviar y corre el inicio del ciclo un intervalo_s.
void avanzarTurno() {
  turno++;
  if (turno == N_TANQUES) {
    armarMedicion();
    turno = 0;
    inicioCiclo += (uint32_t)intervaloS * 1000UL;
  }
  proximaMedicion = instanteTurno(turno);
}

// Mide el tanque que toca y avanza al siguiente turno.
void medirTurno() {
  // Si algo retraso el nodo hasta pasar tambien el turno siguiente, salta los turnos perdidos
  // en vez de medir varios tanques seguidos: cada tanque conserva su lugar en el ciclo.
  uint8_t perdidos = 0;
  while (msHasta(instanteTurno(turno + 1)) <= 0) {   // turno + 1 == 3: el tanque 1 del ciclo siguiente
    avanzarTurno();
    perdidos++;
  }
  if (perdidos > 0) {
    Serial.print(F("[MED] turnos salteados: "));
    Serial.println(perdidos);
  }

  unsigned long inicio = millis();
  guardarMedicion(turno, leerDistanciaMm(turno), inicio);
  avanzarTurno();
}

// medir_ahora: mide los 3 tanques en un hueco entre turnos. No toca proximaMedicion ni
// turno, asi que el ciclo sigue en sus segundos.
void medirTodos() {
  Serial.println(F("[MED] medir_ahora: midiendo los 3 tanques"));
  for (uint8_t t = 0; t < N_TANQUES; t++) {
    unsigned long inicio = millis();
    guardarMedicion(t, leerDistanciaMm(t), inicio);
  }
  medicionPedida = false;
  armarMedicion();
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
  if (nCerca >= MIN_CERCA && nCerca >= nLejos) return DEMASIADO_CERCA;   // tanque lleno
  if (nLejos >= MIN_VALIDAS) return DEMASIADO_LEJOS;
  return SIN_ECO;
}

void imprimirDistancia(uint8_t i, uint16_t d) {
  Serial.print(F("[MED] Tanque "));
  Serial.print(i + 1);
  Serial.print(F(": "));
  if (d == SIN_ECO)              Serial.println(F("sin eco"));
  else if (d == DEMASIADO_CERCA) Serial.println(F("agua en la zona ciega (tanque lleno, se informa 25 cm)"));
  else if (d == DEMASIADO_LEJOS) Serial.println(F("demasiado lejos"));
  else { Serial.print(d / 10.0, 1); Serial.println(F(" cm")); }
}

// ===== MEDICION PERIODICA =====
// Toma una foto de lo que hay en RAM y la deja lista para enviar por el fPort 1.
// Si habia una sin entregar, la reemplaza: la nueva trae datos mas recientes.
void armarMedicion() {
  if (medicion.activo) {
    Serial.println(F("[TX] medicion anterior sin entregar: la reemplaza la nueva"));
  }
  for (uint8_t i = 0; i < N_TANQUES; i++) {
    uint16_t d = ultima[i].hecha ? ultima[i].dist : SIN_ECO;
    medicion.payload[2 * i]     = (d >> 8) & 0xFF;
    medicion.payload[2 * i + 1] = d & 0xFF;
  }
  medicion.intentos       = 0;
  medicion.proximoIntento = millis();
  medicion.activo         = true;
}

// ===== RED Y MEDIR_AHORA: solo en los huecos entre turnos =====
void atenderRed() {
  long libre = msHasta(proximaMedicion);   // ms que faltan para el proximo turno

  // medir_ahora no depende de la red: se mide en cuanto haya un hueco que alcance.
  if (medicionPedida) {
    if (libre >= (long)DURACION_MEDIR_TODOS_MS) medirTodos();
    return;   // hasta medir, no se ocupa el hueco con la red
  }

  if (necesitaUnirse) {
    if (libre >= (long)MARGEN_UNION_MS && msHasta(proximoUnion) <= 0) {
      unirse();
    }
    return;
  }

  // No arrancar un envio que pueda pisar el proximo turno de medicion.
  if (libre < (long)DURACION_INTENTO_MS) return;
  if (node.timeUntilUplink() > 0) return;   // duty cycle

  // Primero los reportes: confirman el comando y son lo que espera quien lo mando.
  if (nReportes > 0 && msHasta(reporteProximoIntento) <= 0) {
    intentarReporte();
  } else if (medicion.activo && msHasta(medicion.proximoIntento) <= 0) {
    intentarMedicion();
  }
}

void intentarMedicion() {
  medicion.intentos++;
  Serial.print(F("[TX] medicion, intento "));
  Serial.print(medicion.intentos);
  Serial.print(F(" ... "));

  if (enviarUplink(PUERTO_MEDICION, medicion.payload, TAM_MEDICION)) {
    medicion.activo = false;
    return;
  }

  unsigned long espera = calcularEspera(medicion.intentos + 1);
  medicion.proximoIntento = millis() + espera;
  Serial.print(F("     reintento en "));
  Serial.print(espera / 1000.0, 1);
  Serial.println(F(" s (si hay hueco antes del proximo turno)"));
}

void intentarReporte() {
  const Reporte& r = reportes[0];
  uint8_t payload[TAM_REPORTE] = {
    r.ref, r.resultado, (uint8_t)((r.intervaloS >> 8) & 0xFF), (uint8_t)(r.intervaloS & 0xFF)
  };

  reporteIntentos++;
  Serial.print(F("[TX] reporte ref="));
  Serial.print(r.ref);
  Serial.print(F(" resultado="));
  Serial.print(r.resultado);
  Serial.print(F(" intento "));
  Serial.print(reporteIntentos);
  Serial.print(F(" ... "));

  bool entregado = enviarUplink(PUERTO_REPORTE, payload, TAM_REPORTE);
  if (!entregado && reporteIntentos < MAX_INTENTOS_REPORTE) {
    reporteProximoIntento = millis() + calcularEspera(reporteIntentos + 1);
    return;
  }
  if (!entregado) {
    Serial.println(F("[TX] reporte descartado: sin ACK tras varios intentos"));
  }
  // Saca el primero de la cola (entregado o descartado).
  for (uint8_t i = 1; i < nReportes; i++) reportes[i - 1] = reportes[i];
  nReportes--;
  reporteIntentos       = 0;
  reporteProximoIntento = millis();
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

  // Un comando puede llegar aunque el ACK no venga en esa ventana: se procesa igual.
  if (estado > 0 && rxTam > 0) {
    procesarBajada(bajada.fPort, rxBuffer, rxTam);
  }

  if (CONFIRMADO && !bajada.confirming) {
    Serial.println(F("sin ACK del servidor"));
    return false;
  }

  if (estado > 0) {
    Serial.print(F("confirmado, ACK en RX"));
    Serial.println(estado);
  } else {
    Serial.println(F("enviado (sin downlink)"));
  }
  return true;
}

// ===== COMANDOS POR DOWNLINK (fPort 10) =====
void procesarBajada(uint8_t puerto, const uint8_t* datos, size_t tam) {
  if (puerto != PUERTO_COMANDOS) {
    Serial.print(F(">>> downlink en el fPort "));
    Serial.print(puerto);
    Serial.println(F(", ignorado (los comandos van en el 10) <<<"));
    return;
  }
  if (tam < 2) {
    // Sin ref no hay a quien contestar.
    Serial.println(F(">>> comando sin ref, ignorado <<<"));
    return;
  }

  uint8_t op  = datos[0];
  uint8_t ref = datos[1];

  if (op == CMD_MEDIR_AHORA && tam == 2) {
    medicionPedida = true;
    encolarReporte(ref, RES_APLICADO);
    Serial.print(F(">>> medir_ahora ref="));
    Serial.print(ref);
    Serial.println(F(": se mide en el proximo hueco entre turnos <<<"));
    return;
  }

  if (op == CMD_SET_CONFIG && tam == 4) {
    uint16_t pedidoS = ((uint16_t)datos[2] << 8) | datos[3];
    if (pedidoS < INTERVALO_MIN_S || pedidoS > INTERVALO_MAX_S) {
      // Fuera de rango: no se aplica nada (contrato 3.3).
      encolarReporte(ref, RES_FUERA_DE_RANGO);
      Serial.print(F(">>> set_config ref="));
      Serial.print(ref);
      Serial.print(F(" RECHAZADO: intervalo_s="));
      Serial.print(pedidoS);
      Serial.println(F(" fuera de 60..43200 <<<"));
      return;
    }
    bool cambia = (pedidoS != intervaloS);
    intervaloS = pedidoS;
    if (almacen.getUInt(NVS_INTERVALO, 0) != pedidoS) {   // flash: solo si cambio
      almacen.putUInt(NVS_INTERVALO, pedidoS);
    }
    encolarReporte(ref, RES_APLICADO);
    Serial.print(F(">>> set_config ref="));
    Serial.print(ref);
    Serial.print(F(": intervalo_s="));
    Serial.print(pedidoS);
    Serial.println(cambia ? F(" (los turnos se recalculan) <<<") : F(" (sin cambios) <<<"));
    if (cambia) reiniciarCiclo();
    return;
  }

  encolarReporte(ref, RES_COMANDO_DESCONOCIDO);
  Serial.print(F(">>> comando desconocido ref="));
  Serial.print(ref);
  Serial.print(F(" (op 0x"));
  Serial.print(op, HEX);
  Serial.print(F(", "));
  Serial.print((unsigned)tam);
  Serial.println(F(" bytes) <<<"));
}

// El reporte lleva el intervalo vigente en el momento en que se atendio el comando.
// Con la cola llena, el reporte nuevo se pierde (el comando se aplica igual) y AURA ve ese
// comando como pendiente. No se descarta el primero porque puede estar enviandose.
void encolarReporte(uint8_t ref, uint8_t resultado) {
  if (nReportes == MAX_REPORTES) {
    Serial.print(F("[TX] cola de reportes llena: sin reporte para ref="));
    Serial.println(ref);
    return;
  }
  if (nReportes == 0) {
    reporteIntentos       = 0;
    reporteProximoIntento = millis();
  }
  reportes[nReportes].ref        = ref;
  reportes[nReportes].resultado  = resultado;
  reportes[nReportes].intervaloS = intervaloS;
  nReportes++;
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
  almacen.putBytes(NVS_NONCES, node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);

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
  Serial.println(F("  - claves de config_local.h distintas a las del device, o"));
  Serial.println(F("  - DevNonce repetido (pedir a la catedra que resetee los nonces del device)."));
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
  if (s >= INTERVALO_MIN_S && s <= INTERVALO_MAX_S) {   // un valor fuera de rango se ignora
    intervaloS = (uint16_t)s;
    Serial.print(F("intervalo_s recuperado de NVS: "));
    Serial.println(intervaloS);
  }
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
