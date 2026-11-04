// Puerta con RFID + sensor magnetico (simulado con un boton) y cerradura electrica.
// Con tarjeta autorizada (o la orden remota de abrir, que llega por la enroladora) el
// pestillo queda abierto 10 s con un pi pi pi que sube de volumen y de ritmo; si la puerta se
// abre y despues de 15 s sigue abierta, suena un aviso cada vez mas fuerte hasta que se cierre.
// Sistema desarmado (normal): la puerta se puede abrir desde adentro sin que suene nada.
// Dos pasadas de tarjeta autorizada seguidas (la segunda mientras el pestillo sigue abierto por
// la primera, 10 s) lo arman: 10 s con un pitido por segundo para salir y despues, si la
// puerta se abre de cualquier forma, suena la alarma. Una sola pasada de tarjeta autorizada lo
// desarma (y abre el pestillo para entrar). El estado armado se guarda en NVS. Modo
// emergencia (orden remota desde la app): el pestillo queda abierto de forma permanente y
// suena una alarma hasta que la app lo apague; se guarda en NVS y sobrevive a un corte de
// luz. Las tarjetas autorizadas se agregan y borran desde la enroladora por ESP-NOW cifrado
// y se guardan en NVS.
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <Preferences.h>

#define PIN_SCK 9
#define PIN_MISO 10
#define PIN_MOSI 20
#define PIN_SS 21
#define PIN_RST 7
#define PIN_BOBINA 3
#define PIN_PUERTA 4  // boton entre GPIO4 y GND (pull-up interna)
#define PIN_RELE 5    // modulo de rele de la cerradura electrica (Luber 500, 12 V)

// El modulo de rele se activa con LOW. La cerradura es fail-safe: CON corriente queda trabada y
// SIN corriente se abre. Por eso rele activado = cerradura energizada = trabada, y rele suelto =
// pestillo abierto (tambien cuando se corta la luz o la placa se reinicia).
#define NIVEL_RELE_ACTIVO LOW
#define NIVEL_RELE_SUELTO (NIVEL_RELE_ACTIVO == LOW ? HIGH : LOW)

// Con el boton: apretado (LOW) = puerta abierta. Con el reed switch real (cerrado con el
// iman = puerta cerrada) se invierte: poner HIGH.
#define NIVEL_PUERTA_ABIERTA LOW

#define VENTANA_ACCESO_MS 10000  // cuanto queda abierto el pestillo despues de una tarjeta valida
#define RAMPA_ABIERTA_MS 20000   // en cuanto llega al maximo el aviso de puerta que quedo abierta
#define ESPERA_AVISO_MS 15000    // desde que se abre la puerta, silencio antes del aviso de puerta abierta
#define TIEMPO_ARMADO_MS 10000   // cuenta con pitidos para salir despues de armar

// PWM de la bobina: 10 bits, el ciclo de trabajo maneja el volumen (512 = 50% = maximo)
#define BITS_PWM 10
#define VOLUMEN_MAX 512
#define VOLUMEN_MIN 128        // donde arrancan los pitidos del pestillo abierto
#define VOLUMEN_MIN_AVISO 256  // donde arranca el aviso de puerta abierta (tiene que oirse desde el principio)

// Tarjeta maestra: siempre autorizada, no se puede borrar
const byte UID_MAESTRA[4] = {0xAB, 0xE8, 0xB0, 0x22};

#define CANAL_ESPNOW 1
#define MAX_TARJETAS 20

// MAC de la enroladora (la unica que puede cambiar la lista) y claves de ESP-NOW (16 caracteres,
// iguales en las dos placas). Van en config_local.h (copiar config_local.h.example): el repo es
// publico. Sin el, el sketch compila y vigila la puerta, pero no acepta ordenes de ninguna enroladora.
#if __has_include("config_local.h")
#include "config_local.h"
#else
#define MAC_ENROLADOR_BYTES {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#define ESPNOW_PMK "CAMBIAR-PMK-0000"
#define ESPNOW_LMK "CAMBIAR-LMK-0000"
#define SIN_CONFIG_LOCAL
#endif
const uint8_t MAC_ENROLADOR[6] = MAC_ENROLADOR_BYTES;
const char PMK[17] = ESPNOW_PMK;
const char LMK[17] = ESPNOW_LMK;

// --- Mensajes (iguales en enrolador.ino) ---
// ABRIR, EMERGENCIA y FIN_EMERGENCIA no usan uid
enum : uint8_t {
  PEDIDO_AGREGAR = 1, PEDIDO_BORRAR = 2, PEDIDO_ABRIR = 3,
  PEDIDO_EMERGENCIA = 4, PEDIDO_FIN_EMERGENCIA = 5
};
enum : uint8_t {
  RES_AGREGADA = 0, RES_YA_ESTABA = 1, RES_BORRADA = 2, RES_NO_ESTABA = 3,
  RES_LISTA_LLENA = 4, RES_ES_MAESTRA = 5, RES_ABIERTA = 6,
  RES_EMERGENCIA = 7, RES_FIN_EMERGENCIA = 8
};
struct Pedido {
  uint8_t tipo;
  uint8_t secuencia;
  uint8_t largo;
  uint8_t uid[10];
};
struct Respuesta {
  uint8_t secuencia;
  uint8_t resultado;
  uint8_t cantidad;  // tarjetas guardadas despues del pedido
};
// ---

struct Tarjeta {
  uint8_t largo;
  uint8_t uid[10];
};

MFRC522 rfid(PIN_SS, PIN_RST);
Preferences prefs;

Tarjeta tarjetas[MAX_TARJETAS];
uint8_t cantidadTarjetas = 0;

// El callback de ESP-NOW corre en la tarea del WiFi: solo copia, y el loop lo atiende
volatile bool hayPedido = false;
Pedido pedido;

bool alarma = false;
bool puertaAbierta = false;
uint32_t accesoHasta = 0;  // millis() hasta el que el pestillo esta abierto (0 = cerrado)
bool entraron = false;     // la puerta se abrio con el pestillo abierto
uint32_t avisoDesde = 0;   // millis() en que arranco el aviso de puerta abierta (0 = sin aviso)
uint32_t pitidoProximo = 0;
uint32_t pitidoApagarEn = 0;
bool emergencia = false;   // modo emergencia: pestillo abierto y alarma hasta que la app lo apague
bool armada = false;       // armado: abrir la puerta sin tarjeta dispara la alarma (se guarda en NVS)
uint32_t armandoHasta = 0; // millis() en que termina la cuenta para salir (0 = no se esta armando)
uint32_t dobleHasta = 0;   // millis() hasta el que una segunda pasada cuenta como doble (0 = ninguna)

void autorizar();
void iniciarEmergencia();
void terminarEmergencia();

bool antenaPrendida() {
  return (rfid.PCD_ReadRegister(MFRC522::TxControlReg) & 0x03) == 0x03;
}

void imprimirUid(const uint8_t *uid, uint8_t largo) {
  for (byte i = 0; i < largo; i++) Serial.printf(" %02X", uid[i]);
}

bool esMaestra(const uint8_t *uid, uint8_t largo) {
  return largo == 4 && memcmp(uid, UID_MAESTRA, 4) == 0;
}

int buscarTarjeta(const uint8_t *uid, uint8_t largo) {
  for (int i = 0; i < cantidadTarjetas; i++)
    if (tarjetas[i].largo == largo && memcmp(tarjetas[i].uid, uid, largo) == 0) return i;
  return -1;
}

bool esAutorizada(const uint8_t *uid, uint8_t largo) {
  return esMaestra(uid, largo) || buscarTarjeta(uid, largo) >= 0;
}

void cargarTarjetas() {
  prefs.begin("puerta", false);
  emergencia = prefs.getBool("emergencia", false);
  if (emergencia) Serial.println("MODO EMERGENCIA activo (quedo guardado antes del reinicio)");
  armada = prefs.getBool("armada", false);
  Serial.println(armada ? "Sistema ARMADO (quedo guardado antes del reinicio)" : "Sistema desarmado");
  cantidadTarjetas = min<uint8_t>(prefs.getUChar("cantidad", 0), MAX_TARJETAS);
  if (cantidadTarjetas) prefs.getBytes("tarjetas", tarjetas, sizeof(Tarjeta) * cantidadTarjetas);
  Serial.printf("Tarjetas autorizadas guardadas: %d (mas la maestra)\n", cantidadTarjetas);
  for (int i = 0; i < cantidadTarjetas; i++) {
    Serial.print("  -");
    imprimirUid(tarjetas[i].uid, tarjetas[i].largo);
    Serial.println();
  }
}

void guardarTarjetas() {
  prefs.putBytes("tarjetas", tarjetas, sizeof(Tarjeta) * cantidadTarjetas);
  prefs.putUChar("cantidad", cantidadTarjetas);
}

void alRecibir(const esp_now_recv_info_t *info, const uint8_t *datos, int largo) {
  // Solo la enroladora puede cambiar la lista (ademas el par esta cifrado con la LMK)
  if (memcmp(info->src_addr, MAC_ENROLADOR, 6) != 0 || largo != sizeof(Pedido)) return;
  if (hayPedido) return;  // todavia no se atendio el anterior
  memcpy(&pedido, datos, sizeof(Pedido));
  hayPedido = true;
}

void iniciarEspNow() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(CANAL_ESPNOW, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: no arranco ESP-NOW");
    return;
  }
  esp_now_set_pmk((const uint8_t *)PMK);
  esp_now_peer_info_t par = {};
  memcpy(par.peer_addr, MAC_ENROLADOR, 6);
  memcpy(par.lmk, LMK, 16);
  par.channel = CANAL_ESPNOW;
  par.encrypt = true;
  if (esp_now_add_peer(&par) != ESP_OK) Serial.println("ERROR: no se pudo agregar la enroladora como par");
  esp_now_register_recv_cb(alRecibir);
}

uint8_t aplicarPedido(const Pedido &p) {
  if (p.tipo == PEDIDO_ABRIR) {
    Serial.print("Orden remota (app): abrir");
    autorizar();
    return RES_ABIERTA;
  }
  if (p.tipo == PEDIDO_EMERGENCIA) {
    Serial.print("Orden remota: emergencia -> ");
    iniciarEmergencia();
    return RES_EMERGENCIA;
  }
  if (p.tipo == PEDIDO_FIN_EMERGENCIA) {
    Serial.print("Orden remota: fin de la emergencia -> ");
    terminarEmergencia();
    return RES_FIN_EMERGENCIA;
  }
  uint8_t largo = min<uint8_t>(p.largo, sizeof(p.uid));
  int i = buscarTarjeta(p.uid, largo);
  if (p.tipo == PEDIDO_AGREGAR) {
    if (esMaestra(p.uid, largo) || i >= 0) return RES_YA_ESTABA;
    if (cantidadTarjetas >= MAX_TARJETAS) return RES_LISTA_LLENA;
    tarjetas[cantidadTarjetas].largo = largo;
    memcpy(tarjetas[cantidadTarjetas].uid, p.uid, largo);
    cantidadTarjetas++;
    guardarTarjetas();
    return RES_AGREGADA;
  }
  if (esMaestra(p.uid, largo)) return RES_ES_MAESTRA;
  if (i < 0) return RES_NO_ESTABA;
  tarjetas[i] = tarjetas[cantidadTarjetas - 1];  // el orden no importa
  cantidadTarjetas--;
  guardarTarjetas();
  return RES_BORRADA;
}

void atenderEnrolador() {
  if (!hayPedido) return;
  Pedido p = pedido;
  hayPedido = false;
  if (p.tipo < PEDIDO_AGREGAR || p.tipo > PEDIDO_FIN_EMERGENCIA) return;

  Respuesta r = {p.secuencia, aplicarPedido(p), cantidadTarjetas};
  if (p.tipo == PEDIDO_AGREGAR || p.tipo == PEDIDO_BORRAR) {  // las ordenes remotas ya imprimieron lo suyo
    Serial.print(p.tipo == PEDIDO_AGREGAR ? "Enroladora: agregar" : "Enroladora: borrar");
    imprimirUid(p.uid, min<uint8_t>(p.largo, sizeof(p.uid)));
    const char *textos[] = {"agregada", "ya estaba", "borrada", "no estaba", "lista llena", "es la maestra"};
    Serial.printf(" -> %s (%d tarjetas)\n", textos[r.resultado], cantidadTarjetas);
  }
  esp_now_send(MAC_ENROLADOR, (const uint8_t *)&r, sizeof(r));
}

void sonar(uint32_t frecuencia, uint32_t volumen = VOLUMEN_MAX) {
  ledcChangeFrequency(PIN_BOBINA, frecuencia, BITS_PWM);
  ledcWrite(PIN_BOBINA, volumen);
}

void silencio() {
  ledcWrite(PIN_BOBINA, 0);
}

// Moneda de Mario: tarjeta autorizada
void sonidoOk() {
  sonar(988);  delay(80);   // B5
  sonar(1319); delay(300);  // E6
  silencio();
}

// Dos pitidos graves: tarjeta rechazada
void sonidoRechazo() {
  for (int i = 0; i < 2; i++) {
    sonar(400); delay(150);
    silencio(); delay(80);
  }
}

// Tarjeta autorizada u orden remota de abrir: apaga la alarma si estaba sonando; si no,
// abre el pestillo VENTANA_ACCESO_MS.
void autorizar() {
  if (alarma) {
    alarma = false;
    silencio();
    Serial.println(" -> AUTORIZADA: alarma desactivada");
    if (puertaAbierta) avisoDesde = millis();
    sonidoOk();
    return;
  }
  Serial.printf(" -> ACCESO OK: pestillo abierto %d s\n", VENTANA_ACCESO_MS / 1000);
  sonidoOk();
  avisoDesde = 0;
  entraron = puertaAbierta;
  accesoHasta = millis() + VENTANA_ACCESO_MS;
}

// Alarma del modo emergencia: dos tonos lentos, distinta de la sirena de intrusion
void sirenaEmergencia() {
  static uint32_t ultimoCambio = 0;
  static bool agudo = false;
  if (millis() - ultimoCambio >= 500) {
    ultimoCambio = millis();
    agudo = !agudo;
    sonar(agudo ? 1500 : 1000);
  }
}

// Orden remota: pestillo abierto de forma permanente y alarma, hasta terminarEmergencia().
// Se guarda en NVS para que un corte de luz no cierre la puerta en plena emergencia.
void iniciarEmergencia() {
  emergencia = true;
  prefs.putBool("emergencia", true);
  armada = false;  // la emergencia le gana a todo y al terminar el sistema queda desarmado
  armandoHasta = 0;
  prefs.putBool("armada", false);
  alarma = false;
  accesoHasta = 0;
  avisoDesde = 0;
  entraron = false;
  Serial.println("MODO EMERGENCIA: pestillo abierto hasta que lo apaguen");
}

void terminarEmergencia() {
  emergencia = false;
  prefs.putBool("emergencia", false);
  silencio();
  Serial.println("Fin del modo emergencia: pestillo cerrado");
  if (puertaAbierta) {
    Serial.println("AVISO: la puerta quedo abierta");
    avisoDesde = millis();
  }
  sonidoOk();
}

// Tres notas bajando: el sistema quedo armado
void sonidoArmado() {
  sonar(1319); delay(100);
  sonar(988);  delay(100);
  sonar(659);  delay(250);
  silencio();
}

// Segunda pasada seguida con el sistema desarmado: la primera ya abrio el pestillo, aca se
// cancela y empieza la cuenta para salir.
void empezarArmado() {
  accesoHasta = 0;
  avisoDesde = 0;
  entraron = false;
  armandoHasta = millis() + TIEMPO_ARMADO_MS;
  Serial.printf(" -> DOBLE PASADA: armando, %d s para salir\n", TIEMPO_ARMADO_MS / 1000);
  sonidoOk();
}

// Una pasada con el sistema armado (o armandose) lo desarma. Despues la tarjeta sigue su camino
// normal en revisarTarjeta(): corta la sirena si sonaba o abre el pestillo para entrar.
void desarmar() {
  armada = false;
  armandoHasta = 0;
  dobleHasta = 0;  // la pasada que desarma no es la primera de una doble
  prefs.putBool("armada", false);
  silencio();
  pitidoApagarEn = 0;
  Serial.print(" -> sistema DESARMADO");
}

// Termina la cuenta para salir: el sistema queda armado
void revisarArmado() {
  if (!armandoHasta || millis() < armandoHasta) return;
  armandoHasta = 0;
  armada = true;
  prefs.putBool("armada", true);
  silencio();
  pitidoApagarEn = 0;
  Serial.println("SISTEMA ARMADO");
  sonidoArmado();
  if (puertaAbierta) {
    Serial.println("ALARMA: el sistema se armo con la puerta abierta");
    alarma = true;
  }
}

// Sirena sin bloquear: alterna dos tonos cada 250 ms mientras la alarma esta activa
void sirena() {
  static uint32_t ultimoCambio = 0;
  static bool agudo = false;
  if (millis() - ultimoCambio >= 250) {
    ultimoCambio = millis();
    agudo = !agudo;
    sonar(agudo ? 2700 : 1800);
  }
}

// Corta el pitido en curso cuando se cumplen sus 60 ms
void atenderPitido() {
  if (pitidoApagarEn && millis() >= pitidoApagarEn) {
    silencio();
    pitidoApagarEn = 0;
  }
}

// Pi pi pi sin bloquear que sube de volumen y de ritmo desde `desde` hasta `desde + rampaMs`
// y despues sigue al maximo
void pitidos(uint32_t desde, uint32_t rampaMs, uint32_t frecuencia,
             uint32_t intervaloInicial, uint32_t intervaloFinal, uint32_t volumenMin) {
  uint32_t ahora = millis();
  if (ahora < pitidoProximo) return;
  float avance = (float)(ahora - desde) / rampaMs;  // 0 -> 1
  if (avance > 1.0f) avance = 1.0f;
  // El oido percibe el volumen en escala logaritmica: con avance al cuadrado la subida
  // se escucha pareja
  uint32_t volumen = volumenMin + (uint32_t)((VOLUMEN_MAX - volumenMin) * avance * avance);
  uint32_t intervalo = intervaloInicial - (uint32_t)((intervaloInicial - intervaloFinal) * avance);
  sonar(frecuencia, volumen);
  pitidoApagarEn = ahora + 60;
  pitidoProximo = ahora + intervalo;
}

void sonidos() {
  atenderPitido();
  if (emergencia) {
    pitidoApagarEn = 0;  // que el pitido no corte la alarma
    sirenaEmergencia();
  } else if (alarma) {
    pitidoApagarEn = 0;  // que el pitido no corte la sirena
    sirena();
  } else if (armandoHasta) {  // cuenta para salir: un pitido por segundo
    static uint32_t proximoBip = 0;
    if (millis() >= proximoBip) {
      proximoBip = millis() + 1000;
      sonar(2000, VOLUMEN_MIN_AVISO);
      pitidoApagarEn = millis() + 80;
    }
  } else if (accesoHasta) {
    pitidos(accesoHasta - VENTANA_ACCESO_MS, VENTANA_ACCESO_MS, 2700, 1000, 100, VOLUMEN_MIN);
  } else if (avisoDesde && millis() >= avisoDesde) {  // avisoDesde puede estar en el futuro (15 s de silencio)
    pitidos(avisoDesde, RAMPA_ABIERTA_MS, 2000, 800, 150, VOLUMEN_MIN_AVISO);
  }
}

void revisarPestillo() {
  if (!accesoHasta || millis() < accesoHasta) return;
  accesoHasta = 0;
  Serial.println("Pestillo cerrado");
  if (puertaAbierta && !alarma && !avisoDesde) {  // si ya se programo el aviso al abrir, se respeta
    Serial.printf("Puerta abierta: si no se cierra, el aviso suena en %d s\n", ESPERA_AVISO_MS / 1000);
    avisoDesde = millis() + ESPERA_AVISO_MS;
  }
}

// La cerradura esta abierta mientras dure la ventana de acceso o el modo emergencia. Se llama en
// cada vuelta del loop y mueve el rele solo cuando cambia, asi cubre todos los caminos (tarjeta,
// orden remota, vencimiento, cierre de la puerta, emergencia) sin tocar su logica.
void actualizarCerradura() {
  static int abierta = -1;  // -1 = todavia no se escribio el rele
  bool debeAbrir = emergencia || accesoHasta;
  if (debeAbrir == abierta) return;
  abierta = debeAbrir;
  // Cerradura fail-safe: se energiza (rele activado) para trabarla y se suelta para abrirla
  digitalWrite(PIN_RELE, abierta ? NIVEL_RELE_SUELTO : NIVEL_RELE_ACTIVO);
  Serial.println(abierta ? "Cerradura: ABIERTA" : "Cerradura: CERRADA");
}

bool leerPuerta() {
  static bool estable = false;
  static bool ultimaLectura = false;
  static uint32_t cambioEn = 0;
  bool lectura = digitalRead(PIN_PUERTA) == NIVEL_PUERTA_ABIERTA;
  if (lectura != ultimaLectura) {
    ultimaLectura = lectura;
    cambioEn = millis();
  }
  if (millis() - cambioEn > 50) estable = lectura;  // antirrebote
  return estable;
}

void revisarPuerta() {
  bool abierta = leerPuerta();
  if (abierta == puertaAbierta) return;
  puertaAbierta = abierta;
  if (emergencia) {  // el pestillo esta abierto: abrir o cerrar la puerta no dispara nada
    Serial.println(abierta ? "Puerta abierta (modo emergencia)" : "Puerta cerrada (modo emergencia)");
    return;
  }
  if (!abierta) {
    Serial.println("Puerta cerrada");
    avisoDesde = 0;
    if (accesoHasta && entraron) {
      // Ya entraron y cerraron: se traba el pestillo y se cortan los pitidos
      accesoHasta = 0;
      dobleHasta = 0;  // el pestillo ya esta cerrado: una pasada nueva no cuenta como segunda
      Serial.println("Pestillo cerrado (entraron y cerraron la puerta)");
    }
    entraron = false;
    return;
  }
  if (accesoHasta) {
    Serial.printf("Puerta abierta (acceso autorizado): si no se cierra, el aviso suena en %d s\n", ESPERA_AVISO_MS / 1000);
    entraron = true;
    avisoDesde = millis() + ESPERA_AVISO_MS;  // 15 s en silencio antes del aviso
  } else if (alarma) {
    Serial.println("Puerta abierta (con la alarma sonando)");
  } else if (armandoHasta) {
    Serial.println("Puerta abierta (saliendo: el sistema se esta armando)");
  } else if (armada) {
    Serial.println("ALARMA: apertura forzada (sistema armado)");
    alarma = true;
  } else {
    Serial.println("Puerta abierta (sistema desarmado)");
  }
}

void revisarTarjeta() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  Serial.print("UID:");
  for (byte i = 0; i < rfid.uid.size; i++) Serial.printf(" %02X", rfid.uid.uidByte[i]);
  bool autorizada = esAutorizada(rfid.uid.uidByte, rfid.uid.size);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (emergencia) {
    Serial.println(" -> modo emergencia: el pestillo ya esta abierto");
    return;
  }
  if (!autorizada) {
    Serial.println(" -> RECHAZADA");
    sonidoRechazo();
    return;
  }
  if (armada || armandoHasta) {  // una sola pasada desarma (o cancela la cuenta)
    desarmar();
    autorizar();
    return;
  }
  if (dobleHasta && millis() < dobleHasta) {  // segunda pasada con el pestillo todavia abierto
    dobleHasta = 0;
    empezarArmado();
    return;
  }
  dobleHasta = millis() + VENTANA_ACCESO_MS;
  autorizar();
}

void setup() {
  // Primero el nivel de reposo (rele suelto = cerradura abierta, como sin corriente) y despues
  // el modo salida; la cerradura se traba cuando termina de arrancar
  digitalWrite(PIN_RELE, NIVEL_RELE_SUELTO);
  pinMode(PIN_RELE, OUTPUT);
  Serial.begin(115200);
  delay(2000);
  ledcAttach(PIN_BOBINA, 2700, BITS_PWM);
  silencio();
  pinMode(PIN_PUERTA, INPUT_PULLUP);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  rfid.PCD_Init();
  Serial.printf("RC522 listo, VersionReg=0x%02X\n", rfid.PCD_ReadRegister(MFRC522::VersionReg));
  if (!antenaPrendida())
    Serial.println("ATENCION: la antena no quedo prendida; el RC522 se reinicia por falta de alimentacion (revisar 3V3 y GND)");
  cargarTarjetas();
  actualizarCerradura();  // ya se sabe si quedo una emergencia guardada: traba o deja abierta
  iniciarEspNow();
#ifdef SIN_CONFIG_LOCAL
  Serial.println("Sin config_local.h: vigila la puerta pero no acepta ordenes de la enroladora.");
#endif
  Serial.printf("Puerta %s vigilada. Pasá la tarjeta para entrar.\n", WiFi.macAddress().c_str());
}

void loop() {
  static uint32_t ultimoChequeo = 0;
  if (millis() - ultimoChequeo > 5000) {
    ultimoChequeo = millis();
    if (!antenaPrendida()) {
      Serial.println("Antena apagada (el RC522 se reinicio): reinicializando");
      rfid.PCD_Init();
    }
  }
  revisarPuerta();
  revisarTarjeta();
  revisarPestillo();
  revisarArmado();
  atenderEnrolador();
  actualizarCerradura();
  sonidos();
  delay(10);
}
