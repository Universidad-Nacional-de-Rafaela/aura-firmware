/*
 * E2-AZ-TERR-TAN01 - nivel de 3 tanques de agua, terraza del Edificio 2 (LoRaWAN, clase A)
 *
 * Tres sensores ultrasonicos AJ-SR04M miden la distancia al agua de 3 tanques.
 * Implementa el contrato MQTT v4.0 por LoRaWAN (hw_id = eui-<DevEUI>): medicion en el fPort 1,
 * reporte de comandos en el fPort 2 y comandos con "ref" en el fPort 10, como el codec de
 * E1-AZ-TERR-TAN01. El par de este sketch es codec.js, en esta carpeta: si cambia el formato
 * de los bytes de uno, cambia el otro.
 *
 * --- CICLO: MIDE LOS 3 Y MANDA ---
 * Cada intervalo_s (60 s por defecto) se miden los 3 tanques, uno detras del otro (menos de 2 s),
 * y enseguida sale un uplink con los tres. Si set_config cambia intervalo_s, el ciclo arranca de
 * nuevo con el intervalo nuevo. Los ciclos se cuentan desde el primero, sin deriva.
 * Un sensor que no da una medicion valida (sin eco, demasiado lejos) no va en el mensaje: el
 * conector nota que ese tanque dejo de llegar. El agua dentro de la zona ciega (tanque lleno) si va.
 *
 * --- CALIBRACION (cero y lleno, desde AURA con set_config) ---
 * Por tanque: cero = distancia del sensor al agua con el tanque vacio (0 %), lleno = distancia
 * con el tanque lleno (100 %), en mm (cm x 10). Con los dos, el nodo calcula el nivel en % y lo
 * manda junto con la distancia. Sin calibrar, manda solo la distancia.
 *
 * --- RED: SE RECUPERA SOLA ---
 * El join no bloquea: si falla, reintenta con espera creciente (30 s ... 5 min, con algo de azar)
 * y sigue midiendo. Si la red deja de confirmar varios uplinks seguidos (la catedra reseteo la
 * sesion o los nonces, borro y volvio a crear el device, cambio el device profile), el nodo
 * descarta su sesion y hace un join nuevo, sin perder los DevNonce guardados.
 *
 * --- MEMORIA ---
 * Las mediciones viven solo en RAM. En flash (NVS) solo van, y solo cuando cambian: el buffer
 * de nonces del join (evita repetir DevNonce), intervalo_s y la calibracion de cada tanque.
 * La sesion LoRaWAN NO se guarda: tras un reinicio el nodo hace un join nuevo.
 *
 * --- SUBIDA (uplinks confirmados) ---
 * fPort 1, medicion: [0] mascara de tanques (bit0 = tanque 1 ... bit2 = tanque 3), y por cada
 *   tanque de la mascara, en orden: distancia en mm (uint16) + nivel en decimas de % (uint16,
 *   0..1000; 0xFFFF = sin calibrar). Big endian. Con los 3 tanques: 13 bytes.
 *   Distancia 0xFFFE = agua en la zona ciega (tanque lleno): el codec la informa como 25 cm.
 * fPort 2, reporte de un comando (contrato 3.4), 16 bytes:
 *   [0] ref   [1] resultado: 0 aplicado, 1 fuera de rango, 2 comando desconocido,
 *   3 calibracion invalida   [2..3] intervalo_s   [4..9] cero de los tanques 1..3 en mm
 *   [10..15] lleno de los tanques 1..3 en mm (0xFFFF = sin calibrar). Es la config vigente.
 *
 * --- BAJADA (fPort 10; el lorawan-bridge pone el ref) ---
 *   01 ref                  medir_ahora: mide los 3 tanques ya (sin mover el ciclo) y los manda.
 *   02 ref (id hi lo)...    set_config: una entrada por parametro. id 0x01 intervalo_s (s,
 *                           60..43200); 0x11..0x13 cero del tanque 1..3 (mm, 250..6000);
 *                           0x21..0x23 lleno del tanque 1..3 (mm, 250..6000). Se valida todo
 *                           y, si algo no valida, no se aplica nada (contrato 3.3). El cero
 *                           tiene que quedar al menos 100 mm mas lejos que el lleno.
 *   02 ref hi lo            set_config solo de intervalo_s (s), en 4 bytes. Es lo que manda el
 *                           codec cuando el comando trae solo el intervalo: entra en un profile
 *                           con aura_max_downlink_bytes = 4.
 *   Cada comando se contesta con un reporte en el fPort 2. Clase A: el comando llega en la
 *   ventana que sigue al proximo uplink del nodo (hasta intervalo_s despues de encolarlo).
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
const char* NVS_CERO[3]   = {"cero1", "cero2", "cero3"};
const char* NVS_LLENO[3]  = {"lleno1", "lleno2", "lleno3"};

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

// ===== CALIBRACION =====
const uint16_t SIN_CALIBRAR      = 0xFFFF;
const uint16_t SPAN_MIN_MM       = 100;     // el cero tiene que estar al menos 10 cm mas lejos que el lleno
const uint16_t NIVEL_SIN_CALIBRAR = 0xFFFF;
uint16_t ceroMm[N_TANQUES]  = {SIN_CALIBRAR, SIN_CALIBRAR, SIN_CALIBRAR};
uint16_t llenoMm[N_TANQUES] = {SIN_CALIBRAR, SIN_CALIBRAR, SIN_CALIBRAR};

// ===== CICLO DE MEDICION =====
const unsigned long ARRANQUE_CICLO_MS = 1000UL;   // primer ciclo, despues de arrancar o de un set_config
// Lo que puede tardar medir los 3 tanques (5 muestras de hasta ~100 ms cada una), con margen.
const unsigned long DURACION_MEDIR_TODOS_MS = 3000UL;

// Ultima medicion de cada tanque. SOLO EN RAM: la proxima medicion la pisa.
struct Medicion {
  uint16_t      dist;      // mm, o SIN_ECO / DEMASIADO_CERCA / DEMASIADO_LEJOS
  unsigned long instante;  // millis() en que se midio
  bool          hecha;     // false hasta la primera medicion de ese tanque
};
Medicion ultima[N_TANQUES];

unsigned long proximaMedicion = 0;      // millis() del proximo ciclo
bool          medicionPedida  = false;  // llego un medir_ahora y todavia no se midio

// ===== PUERTOS Y COMANDOS (iguales a codec.js) =====
const uint8_t PUERTO_MEDICION = 1;
const uint8_t PUERTO_REPORTE  = 2;
const uint8_t PUERTO_COMANDOS = 10;   // CS_FPORT_COMANDOS del lorawan-bridge

const uint8_t CMD_MEDIR_AHORA = 0x01;  // 01 ref
const uint8_t CMD_SET_CONFIG  = 0x02;  // 02 ref (id hi lo)...

const uint8_t PAR_INTERVALO = 0x01;
const uint8_t PAR_CERO      = 0x10;    // + numero de tanque (0x11..0x13)
const uint8_t PAR_LLENO     = 0x20;    // + numero de tanque (0x21..0x23)

const uint8_t RES_APLICADO             = 0;
const uint8_t RES_FUERA_DE_RANGO       = 1;
const uint8_t RES_COMANDO_DESCONOCIDO  = 2;
const uint8_t RES_CALIBRACION_INVALIDA = 3;

const size_t TAM_MEDICION_MAX = 1 + 4 * N_TANQUES;  // 13 bytes
const size_t TAM_REPORTE      = 4 + 4 * N_TANQUES;  // 16 bytes

// ===== INTERVALO DE MEDICION (set_config) =====
// Con 60 s queda lugar entre ciclos para un uplink con sus reintentos y para el join.
const uint16_t INTERVALO_MIN_S = 60;
const uint16_t INTERVALO_MAX_S = 43200;               // 12 h, igual que el codec
uint16_t       intervaloS      = INTERVALO_MIN_S;     // cada cuanto se miden los 3 tanques

// ===== ENVIOS PENDIENTES =====
// Tanques medidos que todavia no se entregaron. El payload se arma al enviar, con la ultima
// medicion de cada uno: si un reintento se demora hasta el ciclo siguiente, sale la nueva.
uint8_t       medicionMascara        = 0;
uint16_t      medicionIntentos       = 0;
unsigned long medicionProximoIntento = 0;

// Reportes de comandos (contrato 3.4). Una cola chica: en clase A llega a lo sumo un
// comando por uplink, asi que casi nunca hay mas de uno esperando.
struct Reporte {
  uint8_t  ref;
  uint8_t  resultado;
  uint8_t  bytes[TAM_REPORTE];
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
// Uplinks seguidos sin ACK antes de descartar la sesion y hacer un join nuevo, contando los
// reintentos: alcanza para no reiniciar por unos ACK perdidos.
const uint8_t       MAX_FALLOS_SEGUIDOS   = 12;

bool          necesitaUnirse = true;
unsigned long proximoUnion   = 0;
uint8_t       fallosUnion    = 0;
uint8_t       fallosSeguidos = 0;

// ===== PROTOTIPOS =====
void iniciarSensores();
uint16_t leerDistanciaMm(uint8_t i);
void imprimirDistancia(uint8_t i, uint16_t d);
void guardarMedicion(uint8_t t, uint16_t d, unsigned long instante);
bool esValida(uint16_t d);
uint16_t nivelDecimas(uint8_t t, uint16_t d);
void medirCiclo();
void medirTodos();
void reiniciarCiclo();
void atenderRed();
void intentarMedicion();
void intentarReporte();
bool enviarUplink(uint8_t puerto, const uint8_t* payload, size_t tam);
void procesarBajada(uint8_t puerto, const uint8_t* datos, size_t tam);
void aplicarSetConfig(uint8_t ref, const uint8_t* datos, size_t tam);
void encolarReporte(uint8_t ref, uint8_t resultado);
void ponerU16(uint8_t* b, uint16_t v);
unsigned long calcularEspera(uint16_t intento);
void unirse();
void forzarJoin();
void fijarDatarate();
void restaurarNonces();
void cargarConfig();
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
  cargarConfig();

  // El ciclo de medicion arranca ya: no espera al join. El join se intenta
  // en el tiempo entre ciclos (ver atenderRed).
  reiniciarCiclo();
  proximoUnion = millis();
  Serial.println();
}

// ===== LOOP PRINCIPAL =====
void loop() {
  if (msHasta(proximaMedicion) <= 0) {
    medirCiclo();      // la medicion siempre va primero
  } else {
    atenderRed();      // lo demas usa el tiempo entre ciclos
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

// Una medicion que se manda: dentro del rango, o el agua en la zona ciega (tanque lleno).
bool esValida(uint16_t d) {
  return (d >= DIST_MIN_MM && d <= DIST_MAX_MM) || d == DEMASIADO_CERCA;
}

// Guarda la medicion y la deja pendiente de envio. Una que no es valida no se manda: si el
// tanque tenia una anterior sin entregar, se descarta, porque ya no es la ultima.
void guardarMedicion(uint8_t t, uint16_t d, unsigned long instante) {
  ultima[t].dist     = d;       // RAM: pisa la medicion anterior de este tanque
  ultima[t].instante = instante;
  ultima[t].hecha    = true;
  imprimirDistancia(t, d);

  uint8_t bit = 1 << t;
  if (esValida(d)) {
    if (medicionMascara == 0) {
      medicionIntentos       = 0;
      medicionProximoIntento = millis();
    }
    medicionMascara |= bit;
  } else {
    medicionMascara &= ~bit;
  }
}

// Nivel en decimas de %, 0..1000, con la calibracion del tanque. La zona ciega cuenta como
// 250 mm. Se recorta a 0..100 %: el agua apenas por encima del lleno o por debajo del cero
// es el tope de la escala, como en un transmisor de nivel.
uint16_t nivelDecimas(uint8_t t, uint16_t d) {
  if (ceroMm[t] == SIN_CALIBRAR || llenoMm[t] == SIN_CALIBRAR) return NIVEL_SIN_CALIBRAR;
  long dist = (d == DEMASIADO_CERCA) ? DIST_MIN_MM : d;
  long span = (long)ceroMm[t] - (long)llenoMm[t];
  long n    = ((long)ceroMm[t] - dist) * 1000L / span;
  if (n < 0)    n = 0;
  if (n > 1000) n = 1000;
  return (uint16_t)n;
}

// Arranca los ciclos de nuevo: al encender y cuando set_config cambia intervalo_s.
void reiniciarCiclo() {
  proximaMedicion = millis() + ARRANQUE_CICLO_MS;
  Serial.print(F("Ciclo: mide los 3 tanques y manda, cada "));
  Serial.print(intervaloS);
  Serial.println(F(" s"));
}

// Mide los 3 tanques (quedan pendientes de envio, juntos) y programa el ciclo siguiente.
void medirCiclo() {
  unsigned long paso = (uint32_t)intervaloS * 1000UL;
  // El ciclo siguiente se cuenta desde este, no desde millis(): sin deriva. Si algo atraso el
  // nodo mas de un ciclo, se saltean los perdidos en vez de medir varias veces seguidas.
  proximaMedicion += paso;
  uint8_t perdidos = 0;
  while (msHasta(proximaMedicion) <= 0) {
    proximaMedicion += paso;
    perdidos++;
  }
  if (perdidos > 0) {
    Serial.print(F("[MED] ciclos salteados: "));
    Serial.println(perdidos);
  }
  for (uint8_t t = 0; t < N_TANQUES; t++) {
    unsigned long inicio = millis();
    guardarMedicion(t, leerDistanciaMm(t), inicio);
  }
}

// medir_ahora: mide los 3 tanques entre dos ciclos y los deja pendientes para salir juntos.
// No toca el ciclo: el siguiente sale cuando le toca.
void medirTodos() {
  Serial.println(F("[MED] medir_ahora: midiendo los 3 tanques"));
  for (uint8_t t = 0; t < N_TANQUES; t++) {
    unsigned long inicio = millis();
    guardarMedicion(t, leerDistanciaMm(t), inicio);
  }
  medicionPedida = false;
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
  if (d == SIN_ECO)              { Serial.println(F("sin eco (no se manda)")); return; }
  if (d == DEMASIADO_LEJOS)      { Serial.println(F("demasiado lejos (no se manda)")); return; }
  if (d == DEMASIADO_CERCA)      Serial.print(F("agua en la zona ciega (tanque lleno, se informa 25 cm)"));
  else                           { Serial.print(d / 10.0, 1); Serial.print(F(" cm")); }
  uint16_t n = nivelDecimas(i, d);
  if (n != NIVEL_SIN_CALIBRAR) {
    Serial.print(F(", nivel "));
    Serial.print(n / 10.0, 1);
    Serial.print(F(" %"));
  }
  Serial.println();
}

// ===== RED Y MEDIR_AHORA: en el tiempo entre ciclos =====
void atenderRed() {
  long libre = msHasta(proximaMedicion);   // ms que faltan para el proximo ciclo

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

  // No arrancar un envio que pueda pisar el proximo ciclo de medicion.
  if (libre < (long)DURACION_INTENTO_MS) return;
  if (node.timeUntilUplink() > 0) return;   // duty cycle

  // Primero los reportes: confirman el comando y son lo que espera quien lo mando.
  if (nReportes > 0 && msHasta(reporteProximoIntento) <= 0) {
    intentarReporte();
  } else if (medicionMascara != 0 && msHasta(medicionProximoIntento) <= 0) {
    intentarMedicion();
  }
}

void intentarMedicion() {
  uint8_t payload[TAM_MEDICION_MAX];
  uint8_t mascara = medicionMascara;
  size_t  n = 0;
  payload[n++] = mascara;
  for (uint8_t t = 0; t < N_TANQUES; t++) {
    if (!(mascara & (1 << t))) continue;
    ponerU16(&payload[n], ultima[t].dist);
    ponerU16(&payload[n + 2], nivelDecimas(t, ultima[t].dist));
    n += 4;
  }

  medicionIntentos++;
  Serial.print(F("[TX] medicion tanques 0x"));
  Serial.print(mascara, HEX);
  Serial.print(F(", intento "));
  Serial.print(medicionIntentos);
  Serial.print(F(" ... "));

  if (enviarUplink(PUERTO_MEDICION, payload, n)) {
    medicionMascara &= ~mascara;   // lo que se midio mientras tanto sigue pendiente
    medicionIntentos       = 0;
    medicionProximoIntento = millis();
    return;
  }

  unsigned long espera = calcularEspera(medicionIntentos + 1);
  medicionProximoIntento = millis() + espera;
  Serial.print(F("     reintento en "));
  Serial.print(espera / 1000.0, 1);
  Serial.println(F(" s (si hay tiempo antes del proximo ciclo)"));
}

void intentarReporte() {
  const Reporte& r = reportes[0];

  reporteIntentos++;
  Serial.print(F("[TX] reporte ref="));
  Serial.print(r.ref);
  Serial.print(F(" resultado="));
  Serial.print(r.resultado);
  Serial.print(F(" intento "));
  Serial.print(reporteIntentos);
  Serial.print(F(" ... "));

  bool entregado = enviarUplink(PUERTO_REPORTE, r.bytes, TAM_REPORTE);
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

  bool confirmado = false;
  if (estado < RADIOLIB_ERR_NONE) {
    Serial.print(F("ERROR, codigo "));
    Serial.println(estado);
  } else {
    // Un comando puede llegar aunque el ACK no venga en esa ventana: se procesa igual.
    if (estado > 0 && rxTam > 0) {
      procesarBajada(bajada.fPort, rxBuffer, rxTam);
    }
    confirmado = !CONFIRMADO || bajada.confirming;
    if (!confirmado) {
      Serial.println(F("sin ACK del servidor"));
    } else if (estado > 0) {
      Serial.print(F("confirmado, ACK en RX"));
      Serial.println(estado);
    } else {
      Serial.println(F("enviado (sin downlink)"));
    }
  }

  if (confirmado) {
    fallosSeguidos = 0;
    return true;
  }
  if (fallosSeguidos < 255) fallosSeguidos++;
  if (fallosSeguidos >= MAX_FALLOS_SEGUIDOS) {
    forzarJoin();
  }
  return false;
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
    Serial.println(F(": se miden los 3 tanques y se mandan <<<"));
    return;
  }

  if (op == CMD_SET_CONFIG && tam == 4) {
    // Forma corta, solo intervalo_s: se pasa como una entrada "01 hi lo".
    uint8_t entrada[3] = {PAR_INTERVALO, datos[2], datos[3]};
    aplicarSetConfig(ref, entrada, 3);
    return;
  }
  if (op == CMD_SET_CONFIG && tam >= 5 && (tam - 2) % 3 == 0) {
    aplicarSetConfig(ref, datos + 2, tam - 2);
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

// set_config: primero valida TODO sobre una copia y recien despues aplica y guarda
// (contrato 3.3: si algo no valida, no se aplica nada).
void aplicarSetConfig(uint8_t ref, const uint8_t* entradas, size_t tam) {
  uint16_t nuevoIntervalo = intervaloS;
  uint16_t nuevoCero[N_TANQUES], nuevoLleno[N_TANQUES];
  for (uint8_t t = 0; t < N_TANQUES; t++) { nuevoCero[t] = ceroMm[t]; nuevoLleno[t] = llenoMm[t]; }

  for (size_t i = 0; i < tam; i += 3) {
    uint8_t  id = entradas[i];
    uint16_t v  = ((uint16_t)entradas[i + 1] << 8) | entradas[i + 2];
    uint8_t  t  = (id & 0x0F) - 1;
    if (id == PAR_INTERVALO) {
      if (v < INTERVALO_MIN_S || v > INTERVALO_MAX_S) {
        encolarReporte(ref, RES_FUERA_DE_RANGO);
        Serial.println(F(">>> set_config RECHAZADO: intervalo_s fuera de 60..43200 <<<"));
        return;
      }
      nuevoIntervalo = v;
    } else if ((id & 0xF0) == PAR_CERO || (id & 0xF0) == PAR_LLENO) {
      if ((id & 0x0F) < 1 || (id & 0x0F) > N_TANQUES) {
        encolarReporte(ref, RES_COMANDO_DESCONOCIDO);
        Serial.println(F(">>> set_config RECHAZADO: tanque inexistente <<<"));
        return;
      }
      if (v < DIST_MIN_MM || v > DIST_MAX_MM) {
        encolarReporte(ref, RES_FUERA_DE_RANGO);
        Serial.println(F(">>> set_config RECHAZADO: cero o lleno fuera de 25..600 cm <<<"));
        return;
      }
      if ((id & 0xF0) == PAR_CERO) nuevoCero[t] = v; else nuevoLleno[t] = v;
    } else {
      encolarReporte(ref, RES_COMANDO_DESCONOCIDO);
      Serial.print(F(">>> set_config RECHAZADO: parametro desconocido 0x"));
      Serial.print(id, HEX);
      Serial.println(F(" <<<"));
      return;
    }
  }

  // Con los dos definidos, el cero (vacio) tiene que quedar mas lejos que el lleno.
  for (uint8_t t = 0; t < N_TANQUES; t++) {
    if (nuevoCero[t] != SIN_CALIBRAR && nuevoLleno[t] != SIN_CALIBRAR &&
        (long)nuevoCero[t] < (long)nuevoLleno[t] + SPAN_MIN_MM) {
      encolarReporte(ref, RES_CALIBRACION_INVALIDA);
      Serial.print(F(">>> set_config RECHAZADO: en el tanque "));
      Serial.print(t + 1);
      Serial.println(F(" el cero tiene que estar al menos 10 cm mas lejos que el lleno <<<"));
      return;
    }
  }

  // Todo valido: aplicar y guardar en flash solo lo que cambio.
  bool cambiaIntervalo = (nuevoIntervalo != intervaloS);
  intervaloS = nuevoIntervalo;
  if (almacen.getUInt(NVS_INTERVALO, 0) != intervaloS) almacen.putUInt(NVS_INTERVALO, intervaloS);
  for (uint8_t t = 0; t < N_TANQUES; t++) {
    ceroMm[t]  = nuevoCero[t];
    llenoMm[t] = nuevoLleno[t];
    if (ceroMm[t] != SIN_CALIBRAR && almacen.getUShort(NVS_CERO[t], SIN_CALIBRAR) != ceroMm[t]) {
      almacen.putUShort(NVS_CERO[t], ceroMm[t]);
    }
    if (llenoMm[t] != SIN_CALIBRAR && almacen.getUShort(NVS_LLENO[t], SIN_CALIBRAR) != llenoMm[t]) {
      almacen.putUShort(NVS_LLENO[t], llenoMm[t]);
    }
  }
  encolarReporte(ref, RES_APLICADO);

  Serial.print(F(">>> set_config ref="));
  Serial.print(ref);
  Serial.print(F(" aplicado: intervalo_s="));
  Serial.print(intervaloS);
  for (uint8_t t = 0; t < N_TANQUES; t++) {
    Serial.print(F(", tanque "));
    Serial.print(t + 1);
    if (ceroMm[t] == SIN_CALIBRAR || llenoMm[t] == SIN_CALIBRAR) {
      Serial.print(F(" sin calibrar"));
    } else {
      Serial.print(F(" cero "));
      Serial.print(ceroMm[t] / 10.0, 1);
      Serial.print(F(" cm / lleno "));
      Serial.print(llenoMm[t] / 10.0, 1);
      Serial.print(F(" cm"));
    }
  }
  Serial.println(F(" <<<"));
  if (cambiaIntervalo) reiniciarCiclo();
}

void ponerU16(uint8_t* b, uint16_t v) {
  b[0] = (v >> 8) & 0xFF;
  b[1] = v & 0xFF;
}

// El reporte lleva la config vigente en el momento en que se atendio el comando.
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
  Reporte& r = reportes[nReportes];
  r.ref       = ref;
  r.resultado = resultado;
  r.bytes[0]  = ref;
  r.bytes[1]  = resultado;
  ponerU16(&r.bytes[2], intervaloS);
  for (uint8_t t = 0; t < N_TANQUES; t++) {
    ponerU16(&r.bytes[4 + 2 * t], ceroMm[t]);
    ponerU16(&r.bytes[4 + 2 * N_TANQUES + 2 * t], llenoMm[t]);
  }
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

  if (estado == RADIOLIB_LORAWAN_NEW_SESSION || estado == RADIOLIB_LORAWAN_SESSION_RESTORED ||
      estado == RADIOLIB_ERR_NONE) {
    Serial.println(F("OK - join nuevo"));
    necesitaUnirse = false;
    fallosUnion    = 0;
    fallosSeguidos = 0;
    fijarDatarate();
    return;
  }

  // Espera creciente (30 s, 1, 2, 4 min, despues 5 min), con hasta un 20 % de azar para que
  // los nodos que arrancan juntos (despues de un corte de luz) no reintenten sincronizados.
  unsigned long espera = ESPERA_UNION_BASE_MS << (fallosUnion < 4 ? fallosUnion : 4);
  if (espera > ESPERA_UNION_MAX_MS) espera = ESPERA_UNION_MAX_MS;
  espera += random(espera / 5);
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

// La red dejo de confirmar: descarta la sesion y hace un join nuevo. clearSession() no toca
// los DevNonce, asi que el join siguiente usa uno nuevo y ChirpStack lo acepta.
void forzarJoin() {
  Serial.print(F("[RED] "));
  Serial.print(fallosSeguidos);
  Serial.println(F(" uplinks seguidos sin ACK: se descarta la sesion y se hace un join nuevo"));
  node.clearSession();
  necesitaUnirse = true;
  proximoUnion   = millis();
  fallosUnion    = 0;
  fallosSeguidos = 0;
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

// intervalo_s y la calibracion guardados. Un valor fuera de rango (de una version anterior o
// corrupto) se ignora y queda el valor por defecto.
void cargarConfig() {
  if (almacen.isKey(NVS_INTERVALO)) {
    uint32_t s = almacen.getUInt(NVS_INTERVALO, INTERVALO_MIN_S);
    if (s >= INTERVALO_MIN_S && s <= INTERVALO_MAX_S) intervaloS = (uint16_t)s;
  }
  for (uint8_t t = 0; t < N_TANQUES; t++) {
    uint16_t c = almacen.getUShort(NVS_CERO[t], SIN_CALIBRAR);
    uint16_t l = almacen.getUShort(NVS_LLENO[t], SIN_CALIBRAR);
    if (c >= DIST_MIN_MM && c <= DIST_MAX_MM) ceroMm[t] = c;
    if (l >= DIST_MIN_MM && l <= DIST_MAX_MM) llenoMm[t] = l;
    if (ceroMm[t] != SIN_CALIBRAR && llenoMm[t] != SIN_CALIBRAR &&
        (long)ceroMm[t] < (long)llenoMm[t] + SPAN_MIN_MM) {
      ceroMm[t] = llenoMm[t] = SIN_CALIBRAR;   // par inconsistente: mejor sin calibrar
    }
  }
  Serial.print(F("Config: intervalo_s="));
  Serial.print(intervaloS);
  for (uint8_t t = 0; t < N_TANQUES; t++) {
    Serial.print(F(", tanque "));
    Serial.print(t + 1);
    Serial.print((ceroMm[t] != SIN_CALIBRAR && llenoMm[t] != SIN_CALIBRAR) ? F(" calibrado") : F(" sin calibrar"));
  }
  Serial.println();
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
