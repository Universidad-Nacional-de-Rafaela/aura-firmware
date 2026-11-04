// Enroladora de tarjetas: lee una tarjeta con el RC522 y le manda el UID a la puerta por
// ESP-NOW cifrado. Sin boton = agregar; con el boton (GPIO4) apretado = borrar.
// Ademas hace de radio de un gateway que corre en la compu: por el serie recibe lineas
// "GW <n> <mac12hex> <comando>" (abrir, emergencia) y se las pasa a la puerta por el mismo ESP-NOW.
//
// La MAC de la puerta y las claves de ESP-NOW van en config_local.h (copiar config_local.h.example).
// Sin el, el sketch compila pero no se empareja con ninguna puerta.
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>

#define PIN_SCK 9
#define PIN_MISO 10
#define PIN_MOSI 20
#define PIN_SS 21
#define PIN_RST 7
#define PIN_BOBINA 3
#define PIN_BORRAR 4  // boton entre GPIO4 y GND (pull-up interna)

#define CANAL_ESPNOW 1
#define ESPERA_RESPUESTA_MS 1000

// MAC de la placa de la puerta y claves de ESP-NOW (16 caracteres, iguales en las dos placas)
#if __has_include("config_local.h")
#include "config_local.h"
#else
#define MAC_PUERTA_BYTES {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#define ESPNOW_PMK "CAMBIAR-PMK-0000"
#define ESPNOW_LMK "CAMBIAR-LMK-0000"
#define SIN_CONFIG_LOCAL
#endif
const uint8_t MAC_PUERTA[6] = MAC_PUERTA_BYTES;
const char PMK[17] = ESPNOW_PMK;
const char LMK[17] = ESPNOW_LMK;

// --- Mensajes (iguales en puerta.ino) ---
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
  uint8_t cantidad;  // tarjetas guardadas en la puerta despues del pedido
};
// ---

#define BITS_PWM 10
#define VOLUMEN_MAX 512

MFRC522 rfid(PIN_SS, PIN_RST);

uint8_t secuencia = 0;
volatile bool hayRespuesta = false;
Respuesta respuesta;

void sonar(uint32_t frecuencia) {
  ledcChangeFrequency(PIN_BOBINA, frecuencia, BITS_PWM);
  ledcWrite(PIN_BOBINA, VOLUMEN_MAX);
}

void silencio() {
  ledcWrite(PIN_BOBINA, 0);
}

void pitidoLectura() {
  sonar(2700); delay(60);
  silencio();
}

// Moneda de Mario: la puerta acepto el pedido
void sonidoOk() {
  sonar(988);  delay(80);   // B5
  sonar(1319); delay(300);  // E6
  silencio();
}

// Cuatro notas bajando: la puerta borro la tarjeta
void sonidoBorrada() {
  int notas[] = {1568, 1319, 1047, 784};  // G6 E6 C6 G5
  for (int f : notas) { sonar(f); delay(110); }
  silencio();
}

// Dos pitidos cortos: no hizo falta cambiar nada (ya estaba / no estaba)
void sonidoSinCambios() {
  for (int i = 0; i < 2; i++) {
    sonar(1800); delay(70);
    silencio(); delay(70);
  }
}

// Dos pitidos graves: error o la puerta no contesto
void sonidoError() {
  for (int i = 0; i < 2; i++) {
    sonar(400); delay(150);
    silencio(); delay(80);
  }
}

bool antenaPrendida() {
  return (rfid.PCD_ReadRegister(MFRC522::TxControlReg) & 0x03) == 0x03;
}

void alRecibir(const esp_now_recv_info_t *info, const uint8_t *datos, int largo) {
  if (memcmp(info->src_addr, MAC_PUERTA, 6) != 0 || largo != sizeof(Respuesta)) return;
  memcpy(&respuesta, datos, sizeof(Respuesta));
  hayRespuesta = true;
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
  memcpy(par.peer_addr, MAC_PUERTA, 6);
  memcpy(par.lmk, LMK, 16);
  par.channel = CANAL_ESPNOW;
  par.encrypt = true;
  if (esp_now_add_peer(&par) != ESP_OK) Serial.println("ERROR: no se pudo agregar la puerta como par");
  esp_now_register_recv_cb(alRecibir);
}

void imprimirUid(const uint8_t *uid, uint8_t largo) {
  for (byte i = 0; i < largo; i++) Serial.printf(" %02X", uid[i]);
}

void enviar(uint8_t tipo) {
  Pedido pedido = {};
  pedido.tipo = tipo;
  pedido.secuencia = ++secuencia;
  pedido.largo = min<uint8_t>(rfid.uid.size, sizeof(pedido.uid));
  memcpy(pedido.uid, rfid.uid.uidByte, pedido.largo);

  Serial.print(tipo == PEDIDO_AGREGAR ? "AGREGAR" : "BORRAR");
  imprimirUid(pedido.uid, pedido.largo);
  Serial.print(" -> ");

  hayRespuesta = false;
  if (esp_now_send(MAC_PUERTA, (const uint8_t *)&pedido, sizeof(pedido)) != ESP_OK) {
    Serial.println("no se pudo enviar");
    sonidoError();
    return;
  }
  uint32_t inicio = millis();
  while (millis() - inicio < ESPERA_RESPUESTA_MS) {
    if (hayRespuesta && respuesta.secuencia == pedido.secuencia) break;
    delay(10);
  }
  if (!hayRespuesta || respuesta.secuencia != pedido.secuencia) {
    Serial.println("la puerta no contesto");
    sonidoError();
    return;
  }

  switch (respuesta.resultado) {
    case RES_AGREGADA:    Serial.print("agregada");         sonidoOk(); break;
    case RES_BORRADA:     Serial.print("borrada");          sonidoBorrada(); break;
    case RES_YA_ESTABA:   Serial.print("ya estaba");        sonidoSinCambios(); break;
    case RES_NO_ESTABA:   Serial.print("no estaba");        sonidoSinCambios(); break;
    case RES_LISTA_LLENA: Serial.print("lista llena");      sonidoError(); break;
    case RES_ES_MAESTRA:  Serial.print("es la maestra, no se borra"); sonidoError(); break;
    default:              Serial.print("respuesta desconocida"); sonidoError(); break;
  }
  Serial.printf(" (la puerta tiene %d tarjetas)\n", respuesta.cantidad);
}

// --- Modo gateway ---
// Por el serie llega una linea  GW <n> <mac de 12 hex> <comando> [0|1]  y se contesta
//   GW_TX <n>           el pedido salio por ESP-NOW
//   GW_OK <n>           la puerta lo aplico
//   GW_ERR <n> <motivo> no se pudo (los motivos son los del contrato MQTT, seccion 3.3 y 3.4)
// Hoy solo conoce la puerta y los comandos  abrir  y  emergencia 1|0. El resto de lo que
// imprime la placa (logs) no empieza con GW_.
void pedirAPuerta(unsigned n, const char *mac, uint8_t tipo, uint8_t esperado) {
  char macPuerta[13];
  snprintf(macPuerta, sizeof(macPuerta), "%02x%02x%02x%02x%02x%02x", MAC_PUERTA[0], MAC_PUERTA[1],
           MAC_PUERTA[2], MAC_PUERTA[3], MAC_PUERTA[4], MAC_PUERTA[5]);
  if (strcasecmp(mac, macPuerta) != 0) {
    Serial.printf("GW_ERR %u nodo_no_alcanzable\n", n);
    return;
  }
  Pedido pedido = {};
  pedido.tipo = tipo;
  pedido.secuencia = ++secuencia;

  hayRespuesta = false;
  if (esp_now_send(MAC_PUERTA, (const uint8_t *)&pedido, sizeof(pedido)) != ESP_OK) {
    Serial.printf("GW_ERR %u nodo_no_alcanzable\n", n);
    return;
  }
  Serial.printf("GW_TX %u\n", n);
  uint32_t inicio = millis();
  while (millis() - inicio < ESPERA_RESPUESTA_MS) {
    if (hayRespuesta && respuesta.secuencia == pedido.secuencia) break;
    delay(10);
  }
  if (!hayRespuesta || respuesta.secuencia != pedido.secuencia) {
    Serial.printf("GW_ERR %u sin_resultado_del_nodo\n", n);
  } else if (respuesta.resultado == esperado) {
    Serial.printf("GW_OK %u\n", n);
  } else {
    Serial.printf("GW_ERR %u rechazado_por_el_nodo\n", n);
  }
}

void atenderGateway() {
  static char linea[48];
  static uint8_t largo = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (largo < sizeof(linea) - 1) linea[largo++] = c;
      continue;
    }
    if (largo == 0) continue;
    linea[largo] = 0;
    largo = 0;

    unsigned n, activa = 0;
    char mac[16], comando[16];
    int campos = sscanf(linea, "GW %u %15s %15s %u", &n, mac, comando, &activa);
    if (campos < 3) {
      Serial.println("GW_ERR 0 json_invalido");
    } else if (strcmp(comando, "abrir") == 0) {
      pedirAPuerta(n, mac, PEDIDO_ABRIR, RES_ABIERTA);
    } else if (strcmp(comando, "emergencia") == 0 && campos == 4 && activa <= 1) {
      if (activa) pedirAPuerta(n, mac, PEDIDO_EMERGENCIA, RES_EMERGENCIA);
      else        pedirAPuerta(n, mac, PEDIDO_FIN_EMERGENCIA, RES_FIN_EMERGENCIA);
    } else if (strcmp(comando, "emergencia") == 0) {
      Serial.printf("GW_ERR %u parametros_invalidos\n", n);
    } else {
      Serial.printf("GW_ERR %u comando_desconocido\n", n);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  ledcAttach(PIN_BOBINA, 2700, BITS_PWM);
  silencio();
  pinMode(PIN_BORRAR, INPUT_PULLUP);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
  rfid.PCD_Init();
  Serial.printf("RC522 listo, VersionReg=0x%02X\n", rfid.PCD_ReadRegister(MFRC522::VersionReg));
  iniciarEspNow();
#ifdef SIN_CONFIG_LOCAL
  Serial.println("Sin config_local.h: no esta emparejada con ninguna puerta.");
#endif
  Serial.printf("Enroladora %s lista. Pasá una tarjeta para agregarla; con el boton apretado, para borrarla.\n",
                WiFi.macAddress().c_str());
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
  atenderGateway();
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(50);
    return;
  }
  pitidoLectura();
  enviar(digitalRead(PIN_BORRAR) == LOW ? PEDIDO_BORRAR : PEDIDO_AGREGAR);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
