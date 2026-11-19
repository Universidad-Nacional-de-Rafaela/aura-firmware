// Estacion de enrolamiento de NTAG 424 DNA para AURA.
//
// Es lo unico del sistema que CAMBIA la clave de una tarjeta; las puertas solo autentican. Se
// conecta por USB a una PC (script enrolar.py) y no se une a la mesh. Hardware: XIAO ESP32C3 +
// RC522 por SPI (los mismos pines que la puerta).
//
// Protocolo por USB (texto, una linea por orden y una linea de respuesta que empieza con OK o ERR):
//   info                     datos de la placa y de donde esta la clave maestra
//   uid                      espera una tarjeta (10 s) y dice su UID
//   estado <UID>             que clave tiene la tarjeta (no cambia nada)
//   enrolar <UID>            cambia la clave 0 de fabrica por la derivada del UID
//   desenrolar <UID>         la devuelve a la clave de fabrica
//   maestra <64 hexa>        (solo modo banco) carga la clave maestra en la flash
//   auto [on|off]            modo autonomo (boton + buzzer), ver abajo
// Las que modifican piden el UID para actuar SOLO sobre la tarjeta que el operador ya vio con
// `uid` y confirmo: si hay otra en el campo no se le toca.
//
// MODO AUTONOMO (sin PC): con el boton suelto, apoyar una tarjeta la enrola; con el boton apretado
// desde antes (EST_BOTON_MIN_MS) la borra. Cada resultado tiene su sonido (ver estacion_logica.h).
// Si llega una orden por USB, el autonomo espera 30 s para no pisar a la PC.
//
// CLAVE MAESTRA. En produccion vive en un eFuse (ENROLADOR_MAESTRA_EFUSE 1): la placa calcula el
// HMAC sin que la clave salga del chip, y no se puede leer ni cambiar. El modo banco (0) la guarda
// en la flash SIN proteccion: sirve para probar y NO para tarjetas de verdad.
// La tarjeta que se enrola con una clave maestra solo la abren las puertas que tengan LA MISMA.
#ifndef ENROLADOR_MAESTRA_EFUSE
#define ENROLADOR_MAESTRA_EFUSE 0
#endif

#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>
#include "esp_mac.h"
#include "esp_random.h"
#include "bootloader_random.h"
#if ENROLADOR_MAESTRA_EFUSE
#include "esp_hmac.h"
#include "esp_efuse.h"
#endif
#include "../../comun/iso_dep.h"
#include "../../comun/sha256_hmac.h"
#include "enrolador_logica.h"
#include "estacion_logica.h"

static const uint8_t PIN_SCK = 6, PIN_MISO = 10, PIN_MOSI = 20, PIN_SS = 21, PIN_RST = 7;
static const uint8_t PIN_BUZZER = 3, PIN_BOTON = 4;   // buzzer pasivo al pin; boton entre el pin y GND
static const uint32_t ESPERA_TARJETA_MS = 10000;
static const uint32_t PC_MANDA_MS = 30000;   // tras una orden por USB, el modo autonomo espera: la PC manda
static const int INTENTOS_ENLACE = 3;
static uint32_t g_fallos_enlace = 0;

static MFRC522 rc(PIN_SS, PIN_RST);
static Preferences prefs;
static isodep_t g_dep;
static uint8_t g_maestra[32];
static bool g_maestra_cargada = false;
#if ENROLADOR_MAESTRA_EFUSE
static hmac_key_id_t g_hmac_id = HMAC_KEY0;
#endif

// ---------- clave maestra ----------

static void cargar_maestra() {
#if ENROLADOR_MAESTRA_EFUSE
  esp_efuse_block_t bloque;
  g_maestra_cargada = esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &bloque);
  if (g_maestra_cargada) g_hmac_id = (hmac_key_id_t)(bloque - EFUSE_BLK_KEY0);
  // La copia de banco, si quedo en la flash, anula la proteccion del eFuse: se borra.
  prefs.begin("enrol", false);
  prefs.remove("maestra");
  prefs.end();
#else
  prefs.begin("enrol", true);
  g_maestra_cargada = prefs.getBytes("maestra", g_maestra, sizeof(g_maestra)) == sizeof(g_maestra);
  prefs.end();
#endif
}

static bool derivar_clave(void *, const uint8_t *uid, size_t largo, uint8_t out[16]) {
  if (!g_maestra_cargada) return false;
#if ENROLADOR_MAESTRA_EFUSE
  uint8_t h[32];
  if (largo != 4 && largo != 7 && largo != 10) return false;
  if (esp_hmac_calculate(g_hmac_id, uid, largo, h) != ESP_OK) return false;
  memcpy(out, h, 16);
  ntag_borrar(h, sizeof(h));
  return true;
#else
  return clave_de_tarjeta(g_maestra, uid, largo, out) == 0;
#endif
}

static bool azar(void *, uint8_t out[16]) {
  esp_fill_random(out, 16);
  return true;
}

// ---------- lector ----------

// Manda una trama con CRC_A y devuelve la respuesta sin CRC. Negativo si falla.
static int trama(void *, const uint8_t *in, size_t n, uint8_t *out, size_t max) {
  uint8_t tx[ISODEP_MAX_TRAMA], rx[ISODEP_MAX_TRAMA];
  if (n > sizeof(tx) - 2) return -1;
  memcpy(tx, in, n);
  if (rc.PCD_CalculateCRC(tx, n, &tx[n]) != MFRC522::STATUS_OK) return -1;
  byte rl = sizeof(rx);
  if (rc.PCD_TransceiveData(tx, n + 2, rx, &rl, nullptr, 0, false) != MFRC522::STATUS_OK) return -1;
  if (rl < 3) return -1;
  uint8_t crc[2];
  if (rc.PCD_CalculateCRC(rx, rl - 2, crc) != MFRC522::STATUS_OK) return -1;
  if (crc[0] != rx[rl - 2] || crc[1] != rx[rl - 1]) return -1;
  size_t util = rl - 2;
  if (util > max) return -1;
  memcpy(out, rx, util);
  return (int)util;
}

static int apdu_hw(void *, const uint8_t *cmd, size_t n, uint8_t *resp, size_t max) {
  return isodep_apdu(&g_dep, cmd, n, resp, max);
}

// Apaga el campo un rato: la tarjeta se reinicia y deja de estar "activa" en ISO-DEP.
static void reiniciar_campo() {
  rc.PCD_AntennaOff();
  delay(60);
  rc.PCD_AntennaOn();
  delay(10);
}

// El RC522 a veces se reinicia solo (un bajon de alimentacion): sigue contestando la version pero
// pierde la configuracion y la antena queda apagada, y no lee nada. Igual que la puerta, se lo
// revisa y se lo vuelve a levantar. Devuelve true si tuvo que reiniciarlo.
static uint32_t g_reinicios_lector = 0;
static bool asegurar_lector() {
  uint8_t v = rc.PCD_ReadRegister(MFRC522::VersionReg);
  uint8_t tx = rc.PCD_ReadRegister(MFRC522::TxControlReg);
  if ((v == 0x91 || v == 0x92) && (tx & 0x03) == 0x03) return false;
  g_reinicios_lector++;
  rc.PCD_Init();
  delay(50);
  return true;
}

static bool esperar_tarjeta(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    asegurar_lector();
    if (rc.PICC_IsNewCardPresent() && rc.PICC_ReadCardSerial()) return true;
    delay(30);
  }
  return false;
}

// ---------- utilidades ----------

static void hex_a_texto(const uint8_t *b, size_t n, char *out) {
  static const char d[] = "0123456789ABCDEF";
  for (size_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
  out[2 * n] = 0;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static bool texto_a_hex(const String &s, uint8_t *out, size_t n) {
  if (s.length() != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    int a = hexv(s[2 * i]), b = hexv(s[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = (uint8_t)((a << 4) | b);
  }
  return true;
}

static bool exito(enr_resultado_t r) {
  return r == ENR_ENROLADA_OK || r == ENR_YA_ENROLADA || r == ENR_DESENROLADA_OK || r == ENR_YA_DE_FABRICA ||
         r == ENR_ESTADO_ENROLADA || r == ENR_ESTADO_DE_FABRICA;
}

// ---------- ordenes ----------

static void orden_info() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char m[13];
  hex_a_texto(mac, 6, m);
  Serial.printf("OK info placa=%s rc522=0x%02X maestra=%s cargada=%s reinicios_lector=%lu fallos_enlace=%lu\n", m,
                rc.PCD_ReadRegister(MFRC522::VersionReg), ENROLADOR_MAESTRA_EFUSE ? "efuse" : "banco",
                g_maestra_cargada ? "si" : "no", (unsigned long)g_reinicios_lector, (unsigned long)g_fallos_enlace);
}

static void orden_uid() {
  if (!esperar_tarjeta(ESPERA_TARJETA_MS)) { Serial.println("ERR sin_tarjeta"); return; }
  char u[21];
  hex_a_texto(rc.uid.uidByte, rc.uid.size, u);
  Serial.printf("OK uid %s\n", u);
  reiniciar_campo();
}

// La tarjeta ya esta seleccionada (rc.uid es el UID). Hace RATS y la accion, reintentando solo
// cuando no se llego a tocarla. Deja el campo como esta: quien llama lo reinicia.
static enr_resultado_t ejecutar_sobre_tarjeta(const String &que, const uint8_t *uid) {
  g_dep = {trama, nullptr, 0, 0};
  if (!isodep_rats(&g_dep)) return ENR_ERR_COMUNICACION;
  enr_ops_t ops = {apdu_hw, derivar_clave, azar, nullptr};
  enr_resultado_t r = ENR_ERR_COMUNICACION;
  // ENR_ERR_COMUNICACION quiere decir "no se llego a tocar la tarjeta": reintentar no tiene riesgo.
  // Cualquier otro resultado (incluidos INCIERTA y SIN_CAMBIO) NO se reintenta a ciegas.
  for (int intento = 1; intento <= INTENTOS_ENLACE && r == ENR_ERR_COMUNICACION; intento++) {
    if (intento > 1) {
      reiniciar_campo();
      if (!esperar_tarjeta(3000) || memcmp(rc.uid.uidByte, uid, NTAG_UID_BYTES) != 0) break;
      g_dep = {trama, nullptr, 0, 0};
      if (!isodep_rats(&g_dep)) continue;
    }
    if (que == "estado") r = enr_estado(&ops, uid, NTAG_UID_BYTES);
    else if (que == "enrolar") r = enr_enrolar(&ops, uid, NTAG_UID_BYTES);
    else r = enr_desenrolar(&ops, uid, NTAG_UID_BYTES);
    if (r == ENR_ERR_COMUNICACION) g_fallos_enlace++;
  }
  return r;
}

// estado / enrolar / desenrolar sobre la tarjeta cuyo UID se paso
static void orden_sobre_tarjeta(const String &que, const String &uid_txt) {
  uint8_t uid[10];
  if (!texto_a_hex(uid_txt, uid, NTAG_UID_BYTES)) { Serial.println("ERR uid_invalido"); return; }
  if (!g_maestra_cargada) { Serial.println("ERR sin_maestra"); return; }
  if (!esperar_tarjeta(ESPERA_TARJETA_MS)) { Serial.println("ERR sin_tarjeta"); return; }
  if (rc.uid.size != NTAG_UID_BYTES || memcmp(rc.uid.uidByte, uid, NTAG_UID_BYTES) != 0) {
    char u[21];
    hex_a_texto(rc.uid.uidByte, rc.uid.size, u);
    Serial.printf("ERR uid_distinto %s\n", u);       // hay otra tarjeta en el campo: no se le toca
    reiniciar_campo();
    return;
  }
  enr_resultado_t r = ejecutar_sobre_tarjeta(que, uid);
  Serial.printf("%s %s %s\n", exito(r) ? "OK" : "ERR", que.c_str(), enr_texto(r));
  reiniciar_campo();
}


// Mide el enlace con la tarjeta SIN autenticar (no gasta intentos): N veces reinicia el campo, hace
// RATS y SELECT de la aplicacion. Dice cuantas salieron bien.
static void orden_enlace(const String &arg) {
  int n = arg.toInt();
  if (n < 1 || n > 100) n = 20;
  if (!esperar_tarjeta(ESPERA_TARJETA_MS)) { Serial.println("ERR sin_tarjeta"); return; }
  int ok_ats = 0, ok_sel = 0, sin_tarjeta = 0;
  static const uint8_t seleccionar[] = {0x00, 0xA4, 0x04, 0x0C, 0x07, 0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01, 0x00};
  for (int i = 0; i < n; i++) {
    reiniciar_campo();
    asegurar_lector();
    if (!esperar_tarjeta(1500)) { sin_tarjeta++; continue; }
    g_dep = {trama, nullptr, 0, 0};
    if (!isodep_rats(&g_dep)) continue;
    ok_ats++;
    uint8_t resp[16];
    int r = isodep_apdu(&g_dep, seleccionar, sizeof(seleccionar), resp, sizeof(resp));
    if (r == 2 && resp[0] == 0x90 && resp[1] == 0x00) ok_sel++;
  }
  reiniciar_campo();
  Serial.printf("OK enlace intentos=%d sin_tarjeta=%d ats=%d select=%d reinicios_lector=%lu\n", n, sin_tarjeta, ok_ats,
                ok_sel, (unsigned long)g_reinicios_lector);
}

static void orden_maestra(const String &hex) {
#if ENROLADOR_MAESTRA_EFUSE
  (void)hex;
  Serial.println("ERR modo_efuse");
#else
  uint8_t k[32];
  if (!texto_a_hex(hex, k, sizeof(k))) { Serial.println("ERR maestra_invalida"); return; }
  prefs.begin("enrol", false);
  bool ok = prefs.putBytes("maestra", k, sizeof(k)) == sizeof(k);
  prefs.end();
  ntag_borrar(k, sizeof(k));
  cargar_maestra();
  Serial.println(ok ? "OK maestra guardada" : "ERR maestra_no_guardada");
#endif
}

// ---------- modo autonomo: boton y buzzer, sin PC ----------

static bool g_auto = true;
static uint32_t g_pc_hasta = 0;      // hasta cuando manda la PC (millis)
static est_boton_t g_boton = {false, 0};
static bool g_esperando_retiro = false;
static int g_ausencias = 0;
static uint32_t g_proximo_retiro = 0, g_libre_desde = 0;

static void nota(uint16_t hz, uint16_t ms) {
  tone(PIN_BUZZER, hz, ms);
  delay(ms + 25);
}

static void sonar(est_sonido_t s) {
  switch (s) {
    case EST_SON_ENROLADA: nota(880, 110); nota(1320, 180); break;
    case EST_SON_BORRADA: nota(1320, 110); nota(660, 180); break;
    case EST_SON_YA: nota(1000, 90); break;
    case EST_SON_ERROR: nota(220, 600); break;
    case EST_SON_PELIGRO:
      for (int i = 0; i < 3; i++) { nota(250, 250); nota(1500, 250); }
      break;
  }
  noTone(PIN_BUZZER);
}

// Una tarjeta ya atendida no se vuelve a atender hasta que se retire. Se la deja en HALT (no
// contesta el REQA) y se mira si sigue ahi despertandola (WUPA).
static void dormir_tarjeta() {
  reiniciar_campo();
  if (rc.PICC_IsNewCardPresent() && rc.PICC_ReadCardSerial()) rc.PICC_HaltA();
}

static bool tarjeta_sigue() {
  byte atqa[2];
  byte n = sizeof(atqa);
  if (rc.PICC_WakeupA(atqa, &n) != MFRC522::STATUS_OK) return false;
  if (rc.PICC_ReadCardSerial()) rc.PICC_HaltA();
  else reiniciar_campo();
  return true;
}

static void paso_autonomo() {
  uint32_t ahora = millis();
  est_boton_actualizar(&g_boton, digitalRead(PIN_BOTON) == LOW, ahora);
  if (!g_auto || (int32_t)(g_pc_hasta - ahora) > 0) return;

  if (g_esperando_retiro) {
    // Se mira cada EST_PERIODO_RETIRO_MS (no en cada vuelta del loop): una falla suelta de lectura no
    // puede dar por retirada a una tarjeta que sigue ahi, y borrar/enrolar dos veces la misma es un riesgo.
    if ((int32_t)(ahora - g_proximo_retiro) < 0) return;
    g_proximo_retiro = ahora + EST_PERIODO_RETIRO_MS;
    if (est_retirada(&g_ausencias, tarjeta_sigue())) {
      g_esperando_retiro = false;
      g_libre_desde = ahora + EST_PAUSA_TRAS_RETIRO_MS;
    }
    return;
  }
  if ((int32_t)(ahora - g_libre_desde) < 0) return;
  if (!rc.PICC_IsNewCardPresent() || !rc.PICC_ReadCardSerial()) return;

  // Hay una tarjeta nueva. El modo se decide AHORA, con el boton como esta en este momento.
  est_modo_t modo = est_modo(&g_boton, ahora);
  char u[21];
  hex_a_texto(rc.uid.uidByte, rc.uid.size, u);
  est_sonido_t sonido = EST_SON_ERROR;
  enr_resultado_t r = ENR_UID_INVALIDO;
  if (!g_maestra_cargada) {
    r = ENR_ERR_SERVICIO;
  } else if (rc.uid.size == NTAG_UID_BYTES) {
    uint8_t uid[NTAG_UID_BYTES];
    memcpy(uid, rc.uid.uidByte, NTAG_UID_BYTES);
    r = ejecutar_sobre_tarjeta(modo == EST_BORRAR ? "desenrolar" : "enrolar", uid);
  }
  sonido = est_sonido(modo, r);
  Serial.printf("AUTO %s %s %s\n", modo == EST_BORRAR ? "desenrolar" : "enrolar", u, enr_texto(r));
  sonar(sonido);
  dormir_tarjeta();
  g_esperando_retiro = true;
  g_ausencias = 0;
  g_proximo_retiro = millis() + EST_PERIODO_RETIRO_MS;
}

static void orden_auto(const String &arg) {
  if (arg == "on") g_auto = true;
  else if (arg == "off") g_auto = false;
  else if (arg != "") { Serial.println("ERR uso: auto on|off"); return; }
  g_esperando_retiro = false;
  g_pc_hasta = 0;                            // `auto on` es pedirle que arranque ya
  Serial.printf("OK auto %s\n", g_auto ? "on" : "off");
}

static void procesar(String linea) {
  linea.trim();
  if (linea.length() == 0) return;
  g_pc_hasta = millis() + PC_MANDA_MS;       // la PC esta mandando: el modo autonomo espera
  g_esperando_retiro = false;
  int sp = linea.indexOf(' ');
  String cmd = sp < 0 ? linea : linea.substring(0, sp);
  String arg = sp < 0 ? "" : linea.substring(sp + 1);
  arg.trim();
  if (cmd == "info") orden_info();
  else if (cmd == "uid") orden_uid();
  else if (cmd == "estado" || cmd == "enrolar" || cmd == "desenrolar") orden_sobre_tarjeta(cmd, arg);
  else if (cmd == "enlace") orden_enlace(arg);
  else if (cmd == "auto") orden_auto(arg);
  else if (cmd == "maestra") orden_maestra(arg);
  else Serial.println("ERR orden_desconocida (info, uid, estado, enrolar, desenrolar, maestra)");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  bootloader_random_enable();   // sin radio el generador de azar de la C3 no es de verdad: lo activa
  pinMode(PIN_BOTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
  rc.PCD_Init();
  cargar_maestra();
  Serial.printf("Estacion de enrolamiento lista. RC522 0x%02X, maestra %s.\n",
                rc.PCD_ReadRegister(MFRC522::VersionReg), g_maestra_cargada ? "cargada" : "SIN cargar");
}

void loop() {
  static String linea;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') { procesar(linea); linea = ""; }
    else if (c != '\r' && linea.length() < 200) linea += c;
  }
  static uint32_t ultimo = 0;
  if (millis() - ultimo > 500) {
    ultimo = millis();
    asegurar_lector();
  }
  paso_autonomo();
  delay(5);
}
