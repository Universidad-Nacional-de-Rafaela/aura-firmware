/*
 * E1-AZ-TERR-TAN01 — nivel de dos tanques de agua de la terraza del E1, por LoRaWAN.
 *
 * Mide la distancia del sensor a la superficie del agua en cada tanque (mediana de 5
 * lecturas seguidas, para descartar ecos falsos) y la manda
 * cruda: el porcentaje y los litros los calcula el conector en AURA, que conoce las
 * medidas de cada tanque (conectores/E1-AZ-TERR-TAN01/conector.py). Asi un error en
 * una medida se corrige sin reflashear la placa y la historia se puede recalcular.
 *
 * Los bytes son los del codec.js del device profile aura-tanque-2x-ultrasonico:
 *
 * SUBIDA
 *   fPort 1, 4 bytes, medicion (big endian):
 *     0-1  distancia del tanque 1, en cm x 10 (uint16); 0xFFFF = sin lectura
 *     2-3  lo mismo para el tanque 2
 *   fPort 2, 4 bytes, reporte de un comando:
 *     0    ref del comando
 *     1    resultado: 0 aplicado, 1 fuera de rango, 2 comando desconocido
 *     2-3  intervalo de lectura vigente, en segundos (uint16)
 *
 * BAJADA (fPort 10, el firmware no filtra por puerto)
 *   01 ref              medir_ahora: mide y envia sin esperar al intervalo
 *   02 ref hi lo        set_config: intervalo de lectura en s (60 a 43200), en NVS
 *
 * El nodo es clase C: escucha todo el tiempo. Cada comando se contesta con un reporte
 * en el fPort 2; despues de medir_ahora sale ademas la medicion. Un comando que llega
 * dos veces seguidas con el mismo ref no se vuelve a aplicar: solo se repite el
 * reporte. Entre dos uplinks, reportes incluidos, pasan al menos MIN_ENTRE_ENVIOS_MS.
 * Nunca se manda un valor inventado.
 *
 * Las credenciales OTAA van en config_local.h (copiar config_local.h.example). Sin
 * ese archivo el sketch compila y mide (se ve por el monitor serie), pero no se une
 * a la red.
 */

#if __has_include("config_local.h")
#include "config_local.h"
#endif

#include <RadioLib.h>
#include <Preferences.h>

#if defined(LORAWAN_DEV_EUI) && defined(LORAWAN_JOIN_EUI) && defined(LORAWAN_APP_KEY)
#define HAY_CREDENCIALES 1
const uint64_t JOIN_EUI  = LORAWAN_JOIN_EUI;
const uint64_t DEV_EUI   = LORAWAN_DEV_EUI;
uint8_t        APP_KEY[] = LORAWAN_APP_KEY;
#else
#define HAY_CREDENCIALES 0
#endif

// ===== Persistencia de la sesion LoRaWAN (NVS) =====
Preferences almacen;
const char* NVS_ESPACIO = "lorawan";
const char* NVS_NONCES  = "nonces";
const char* NVS_SESION  = "sesion";

// ===== Configuracion remota (NVS, aparte de la sesion) =====
Preferences ajustes;
const char* NVS_AJUSTES   = "config";
const char* NVS_INTERVALO = "intervalo_s";

const uint16_t INTERVALO_DEFECTO_S = 60;
const uint16_t INTERVALO_MIN_S     = 60;
const uint16_t INTERVALO_MAX_S     = 43200;   // 12 h, el uplink interval del device profile
uint16_t intervaloS = INTERVALO_DEFECTO_S;

// ===== Pines del Wio-SX1262 (conector B2B) =====
#define PIN_NSS    41
#define PIN_DIO1   39
#define PIN_NRST   42
#define PIN_BUSY   40
#define PIN_ANT_SW 38
SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY);

// ===== LoRaWAN =====
const LoRaWANBand_t REGION   = AU915;
const uint8_t       SUBBANDA = 2;   // canales 8-15 (au915_1 en ChirpStack)
LoRaWANNode nodo(&radio, &REGION, SUBBANDA);

const uint8_t PUERTO_MEDICION = 1;
const uint8_t PUERTO_REPORTE  = 2;

// ===== Sensores JSN-SR04T =====
const int TRIG[2] = {D1, D5};
const int ECHO[2] = {D2, D4};

const float VELOCIDAD_SONIDO_CM_US = 0.0343;
const unsigned long TIMEOUT_ECO_US = 35000;   // ~600 cm ida y vuelta
const unsigned long PAUSA_ENTRE_SENSORES_MS = 60;   // que el eco de uno no lo lea el otro

// Cada medicion es la mediana de varias lecturas seguidas: un eco falso suelto (se vieron
// picos de ~47 cm en el tanque 1) queda en un extremo y no llega a ser la mediana. Si no
// hay al menos LECTURAS_MIN_VALIDAS buenas, no se manda ese tanque.
const int LECTURAS_POR_MEDICION = 5;
const int LECTURAS_MIN_VALIDAS  = 3;
const unsigned long PAUSA_ENTRE_LECTURAS_MS = 60;

// Por encima de 600 cm la lectura no es confiable: no se manda. Por debajo de 20 cm
// (zona ciega del JSN-SR04T) se manda igual: es el agua por encima de la marca de lleno
// o el sensor obstruido, y el conector avisa con la alerta nivel_alto.
const float DIST_MIN_CM = 0.0;
const float DIST_MAX_CM = 600.0;

const uint16_t SIN_LECTURA = 0xFFFF;

// ===== Temporizacion =====
const unsigned long REINTENTO_JOIN_MS   = 300000UL;   // 5 minutos
const unsigned long MIN_ENTRE_ENVIOS_MS = 10000UL;    // entre dos uplinks cualesquiera
unsigned long ultimaMedicion = 0;
unsigned long ultimoEnvio = 0;
unsigned long ultimoIntentoJoin = 0;
bool unido = false;
bool pedidoMedir = false;   // llego un medir_ahora y todavia no se midio

// ===== Comandos por downlink =====
const uint8_t CMD_MEDIR_AHORA = 0x01;
const uint8_t CMD_SET_CONFIG  = 0x02;

const uint8_t RES_APLICADO       = 0;
const uint8_t RES_FUERA_DE_RANGO = 1;
const uint8_t RES_DESCONOCIDO    = 2;

const size_t MAX_DOWNLINK = 64;

// Reportes que todavia no salieron. Se mandan en loop(), fuera de sendReceive.
struct Reporte { uint8_t ref; uint8_t resultado; };
const int MAX_REPORTES = 4;
Reporte reportes[MAX_REPORTES];
int cantReportes = 0;

// El ultimo comando procesado. El ref va de 1 a 255: 0 es "ninguno todavia".
uint8_t ultimoRef = 0;
uint8_t ultimoResultado = 0;

unsigned long intervaloMs() {
  return (unsigned long)intervaloS * 1000UL;
}

// Distancia en cm, o NAN si el sensor no respondio o la lectura cae fuera de rango.
float medirDistancia(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(5);
  digitalWrite(trig, HIGH);
  delayMicroseconds(20);
  digitalWrite(trig, LOW);

  unsigned long duracion = pulseIn(echo, HIGH, TIMEOUT_ECO_US);
  if (duracion == 0) return NAN;

  float distancia = (duracion * VELOCIDAD_SONIDO_CM_US) / 2.0;
  if (distancia < DIST_MIN_CM || distancia > DIST_MAX_CM) return NAN;
  return distancia;
}

// Mediana de LECTURAS_POR_MEDICION lecturas, o NAN si no hubo LECTURAS_MIN_VALIDAS buenas.
// Las lecturas sin eco o fuera de rango se descartan, no se reemplazan por otro valor.
float medirMediana(int trig, int echo) {
  float v[LECTURAS_POR_MEDICION];
  int n = 0;
  for (int i = 0; i < LECTURAS_POR_MEDICION; i++) {
    if (i > 0) delay(PAUSA_ENTRE_LECTURAS_MS);
    float d = medirDistancia(trig, echo);
    if (!isnan(d)) v[n++] = d;
  }
  if (n < LECTURAS_MIN_VALIDAS) return NAN;

  for (int i = 1; i < n; i++) {   // orden por insercion: n <= 5
    float x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
  if (n % 2 == 1) return v[n / 2];
  return (v[n / 2 - 1] + v[n / 2]) / 2.0f;
}

uint16_t codificar(float distancia) {
  if (isnan(distancia)) return SIN_LECTURA;
  return (uint16_t)lroundf(distancia * 10.0f);   // 600 cm -> 6000, lejos de 0xFFFF
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) { }
  Serial.println(F("\n=== E1-AZ-TERR-TAN01 - nivel de tanques (LoRaWAN) ==="));

  for (int i = 0; i < 2; i++) {
    pinMode(TRIG[i], OUTPUT);
    pinMode(ECHO[i], INPUT);
  }
  // el JSN-SR04T tarda en arrancar: sin esto la primera lectura da sin lectura
  delay(500);

  // Un valor guardado fuera de rango (NVS corrupta) no se usa: queda el de fabrica.
  ajustes.begin(NVS_AJUSTES, false);
  uint16_t guardado = ajustes.getUShort(NVS_INTERVALO, INTERVALO_DEFECTO_S);
  if (guardado >= INTERVALO_MIN_S && guardado <= INTERVALO_MAX_S) intervaloS = guardado;
  Serial.printf("Intervalo de lectura: %u s\n", intervaloS);

  pinMode(PIN_ANT_SW, OUTPUT);
  digitalWrite(PIN_ANT_SW, HIGH);

  Serial.print(F("Iniciando radio SX1262... "));
  int estado = radio.begin();
  if (estado != RADIOLIB_ERR_NONE) {
    Serial.printf("ERROR %d, reinicio en 10 s\n", estado);
    delay(10000);
    ESP.restart();
  }
  Serial.println(F("OK"));
  radio.setDio2AsRfSwitch(true);

#if HAY_CREDENCIALES
  almacen.begin(NVS_ESPACIO, false);
  unirse();
#else
  Serial.println(F("Sin config_local.h: mide pero no se une a la red."));
#endif

  ultimaMedicion = millis() - intervaloMs();   // la primera medicion sale ya
}

void loop() {
#if HAY_CREDENCIALES
  if (!unido && millis() - ultimoIntentoJoin >= REINTENTO_JOIN_MS) unirse();
  if (unido) revisarDownlinkClaseC();
#endif
  bool puedeEnviar = millis() - ultimoEnvio >= MIN_ENTRE_ENVIOS_MS;

  // El reporte va antes que la medicion: es lo que confirma el comando.
  if (unido && cantReportes > 0) {
    if (puedeEnviar) enviarReporte();
  } else if (puedeEnviar && (pedidoMedir || millis() - ultimaMedicion >= intervaloMs())) {
    pedidoMedir = false;
    ultimaMedicion = millis();
    medirYEnviar();
  }
  delay(50);
}

void encolarReporte(uint8_t ref, uint8_t resultado) {
  if (cantReportes == MAX_REPORTES) {
    Serial.printf("[RX] sin lugar para el reporte de ref %u: se descarta\n", ref);
    return;
  }
  reportes[cantReportes++] = {ref, resultado};
}

// set_config: valida antes de aplicar; si no valida, no cambia nada.
uint8_t aplicarIntervalo(uint16_t segundos) {
  if (segundos < INTERVALO_MIN_S || segundos > INTERVALO_MAX_S) return RES_FUERA_DE_RANGO;
  intervaloS = segundos;
  ajustes.putUShort(NVS_INTERVALO, intervaloS);
  return RES_APLICADO;
}

// Un downlink llega en la ventana de clase C o en RX1/RX2 despues de un uplink.
// El puerto 0 trae solo comandos MAC de la red, sin datos para la aplicacion.
// Sin ref (menos de 2 bytes) no hay a quien contestarle: se ignora.
// Desde aca no se envia nada: se encola el reporte y loop() hace los envios.
void procesarDownlink(const uint8_t* datos, size_t largo, uint8_t puerto) {
  if (puerto == 0 || largo == 0) return;
  if (largo < 2) {
    Serial.printf("[RX] downlink de %u byte sin ref (puerto %u): se ignora\n",
                  (unsigned)largo, puerto);
    return;
  }
  uint8_t cmd = datos[0];
  uint8_t ref = datos[1];

  // El mismo ref dos veces seguidas es el mismo comando repetido: se contesta
  // de nuevo con el resultado que tuvo, sin volver a aplicarlo.
  if (ref == ultimoRef) {
    Serial.printf("[RX] ref %u repetido: se repite el reporte sin aplicar\n", ref);
    encolarReporte(ref, ultimoResultado);
    return;
  }

  uint8_t res;
  if (cmd == CMD_MEDIR_AHORA) {
    Serial.printf("[RX] medir_ahora (ref %u)\n", ref);
    pedidoMedir = true;
    res = RES_APLICADO;
  } else if (cmd == CMD_SET_CONFIG && largo >= 4) {
    uint16_t segundos = (datos[2] << 8) | datos[3];
    res = aplicarIntervalo(segundos);
    Serial.printf("[RX] set_config intervalo_s=%u (ref %u): %s\n", segundos, ref,
                  res == RES_APLICADO ? "aplicado" : "fuera de rango, no se aplica");
  } else {
    Serial.printf("[RX] comando desconocido 0x%02X (%u bytes, ref %u)\n",
                  cmd, (unsigned)largo, ref);
    res = RES_DESCONOCIDO;
  }
  ultimoRef = ref;
  ultimoResultado = res;
  encolarReporte(ref, res);
}

void revisarDownlinkClaseC() {
  uint8_t datos[MAX_DOWNLINK];
  size_t largo = 0;
  LoRaWANEvent_t evento;
  // getDownlinkClassC no consulta la radio: solo mira si la interrupcion marco algo
  if (nodo.getDownlinkClassC(datos, &largo, &evento) > 0) {
    procesarDownlink(datos, largo, evento.fPort);
  }
}

// Manda un uplink, procesa lo que baje en RX1/RX2 y guarda la sesion.
void enviar(const uint8_t* payload, size_t largo, uint8_t puerto) {
  Serial.printf("[TX] Enviando %u bytes por el puerto %u... ", (unsigned)largo, puerto);
  uint8_t bajada[MAX_DOWNLINK];
  size_t largoBajada = 0;
  LoRaWANEvent_t eventoBajada;
  int estado = nodo.sendReceive(payload, largo, puerto, bajada, &largoBajada,
                                false, NULL, &eventoBajada);
  if (estado >= RADIOLIB_ERR_NONE) Serial.println(F("OK"));
  else Serial.printf("ERROR %d\n", estado);
  if (estado > 0) procesarDownlink(bajada, largoBajada, eventoBajada.fPort);   // > 0: llego un downlink
  ultimoEnvio = millis();

  guardarBuffer(NVS_SESION, nodo.getBufferSession(), RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
}

// Saca el primer reporte de la cola antes de enviarlo: si en RX1/RX2 llega otro
// comando, su reporte se encola atras y sale en la proxima vuelta.
void enviarReporte() {
  Reporte r = reportes[0];
  for (int i = 1; i < cantReportes; i++) reportes[i - 1] = reportes[i];
  cantReportes--;

  uint8_t payload[4] = {r.ref, r.resultado, (uint8_t)(intervaloS >> 8), (uint8_t)(intervaloS & 0xFF)};
  Serial.printf("[TX] Reporte ref %u, resultado %u, intervalo %u s\n", r.ref, r.resultado, intervaloS);
  enviar(payload, sizeof(payload), PUERTO_REPORTE);
}

void medirYEnviar() {
  float d[2];
  d[0] = medirMediana(TRIG[0], ECHO[0]);
  delay(PAUSA_ENTRE_SENSORES_MS);
  d[1] = medirMediana(TRIG[1], ECHO[1]);

  uint8_t payload[4];
  for (int i = 0; i < 2; i++) {
    uint16_t v = codificar(d[i]);
    payload[2 * i]     = v >> 8;
    payload[2 * i + 1] = v & 0xFF;
    if (isnan(d[i])) Serial.printf("Tanque %d: sin lectura\n", i + 1);
    else             Serial.printf("Tanque %d: %.1f cm\n", i + 1, d[i]);
  }

  if (isnan(d[0]) && isnan(d[1])) {
    Serial.println(F("[TX] Ningun sensor respondio: no se envia."));
    return;
  }
  if (!unido) {
    Serial.println(F("[TX] Sin unirse a la red: no se envia."));
    return;
  }
  enviar(payload, sizeof(payload), PUERTO_MEDICION);
}

#if HAY_CREDENCIALES
// Join OTAA, restaurando la sesion guardada si la hay. Si falla, loop() reintenta
// cada REINTENTO_JOIN_MS en vez de dejar el nodo colgado en la terraza.
void unirse() {
  ultimoIntentoJoin = millis();
  Serial.print(F("Uniendo a la red (OTAA)... "));

  // LoRaWAN 1.0.x: nwkKey en nullptr. Un puntero no nulo pone a RadioLib en modo 1.1
  // y el join falla.
  nodo.beginOTAA(JOIN_EUI, DEV_EUI, nullptr, APP_KEY);
  restaurarBuffer(NVS_NONCES, RADIOLIB_LORAWAN_NONCES_BUF_SIZE, true);
  restaurarBuffer(NVS_SESION, RADIOLIB_LORAWAN_SESSION_BUF_SIZE, false);

  int estado = nodo.activateOTAA();
  guardarBuffer(NVS_NONCES, nodo.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  if (estado != RADIOLIB_LORAWAN_NEW_SESSION && estado != RADIOLIB_LORAWAN_SESSION_RESTORED) {
    Serial.printf("ERROR %d, reintento en %lu s\n", estado, REINTENTO_JOIN_MS / 1000);
    return;
  }
  unido = true;
  Serial.println(estado == RADIOLIB_LORAWAN_NEW_SESSION ? F("OK (sesion nueva)") : F("OK (sesion restaurada)"));

  // En LoRaWAN 1.0.x el cambio de clase es local: el device profile de ChirpStack
  // tiene que tener clase C habilitada para mandar downlinks fuera de RX1/RX2.
  estado = nodo.setClass(RADIOLIB_LORAWAN_CLASS_C);
  if (estado == RADIOLIB_ERR_NONE) Serial.println(F("Clase C: escuchando downlinks"));
  else Serial.printf("Clase C: ERROR %d, sigue en clase A\n", estado);
}

bool restaurarBuffer(const char* clave, size_t tam, bool esNonces) {
  if (!almacen.isKey(clave)) return false;
  uint8_t buffer[tam];
  if (almacen.getBytes(clave, buffer, tam) != tam) return false;
  int16_t estado = esNonces ? nodo.setBufferNonces(buffer) : nodo.setBufferSession(buffer);
  return estado == RADIOLIB_ERR_NONE;
}
#endif

void guardarBuffer(const char* clave, const uint8_t* datos, size_t tam) {
  almacen.putBytes(clave, datos, tam);
}
