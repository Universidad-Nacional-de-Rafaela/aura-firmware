#include "ld2420.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "aura_tiempo.h"

#define LD2420_UART_BUF 512
#define LD2420_REPORT_MODE_NATIVE 100UL

// ===== Tramas de comando del LD2420 (ver docs/PROTOCOL.md de la libreria) =====
static void escribir16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void escribir32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// Envuelve un comando en la trama FD FC FB FA ... 04 03 02 01.
static size_t trama_encode(uint16_t comando, const uint8_t* payload, size_t n,
                           uint8_t* salida, size_t cap) {
  if ((n > 0 && payload == NULL) || salida == NULL || n > 80 || cap < n + 12U) return 0;
  salida[0] = 0xFD; salida[1] = 0xFC; salida[2] = 0xFB; salida[3] = 0xFA;
  escribir16(salida + 4, (uint16_t)(n + 2U));
  escribir16(salida + 6, comando);
  for (size_t i = 0; i < n; i++) salida[8 + i] = payload[i];
  size_t pie = 8U + n;
  salida[pie] = 0x04; salida[pie + 1] = 0x03; salida[pie + 2] = 0x02; salida[pie + 3] = 0x01;
  return n + 12U;
}

static void uart_drenar(uart_port_t uart) {
  uint8_t b;
  while (uart_read_bytes(uart, &b, 1, 0) == 1) { }
}

static void mandar_comando(ld2420_t* r, uint16_t comando, const uint8_t* payload, size_t n) {
  uint8_t frame[32];
  size_t len = trama_encode(comando, payload, n, frame, sizeof(frame));
  if (len == 0) return;
  uart_write_bytes(r->uart, (const char*)frame, len);
  uart_wait_tx_done(r->uart, pdMS_TO_TICKS(100));
  vTaskDelay(pdMS_TO_TICKS(100));
  uart_drenar(r->uart);
}

// Manda el modo "reporte nativo simple" (ON/OFF/Range). Ver
// configureReportModeUnchecked() de la libreria original.
static void configurar_modo_nativo(ld2420_t* r) {
  uart_drenar(r->uart);
  const uint8_t enable[2] = {0x01, 0x00};
  mandar_comando(r, 0x00FF, enable, sizeof(enable));

  uint8_t pay[6];
  escribir16(pay, 0);
  escribir32(pay + 2, LD2420_REPORT_MODE_NATIVE);
  mandar_comando(r, 0x0012, pay, sizeof(pay));

  mandar_comando(r, 0x00FE, NULL, 0);
}

// ===== Parser de lineas nativas =====
static void limpiar_linea(ld2420_t* r) {
  r->line_size = 0;
  r->line[0] = '\0';
  r->overflowed = false;
}

static void rechazar_linea(ld2420_t* r) {
  r->rejected_frames++;
  r->has_presence = false;
  limpiar_linea(r);
}

// Procesa la linea terminada en '\n'. Devuelve true si hay reporte completo.
static bool terminar_linea(ld2420_t* r, bool* detectado, uint16_t* distancia) {
  if (r->overflowed) { rechazar_linea(r); return false; }
  if (r->line_size == 0) { limpiar_linea(r); return false; }
  if (strcmp(r->line, "ON") == 0) {
    r->has_presence = true;
    r->pending_presence = true;
    limpiar_linea(r);
    return false;
  }
  if (strcmp(r->line, "OFF") == 0) {
    r->has_presence = true;
    r->pending_presence = false;
    limpiar_linea(r);
    return false;
  }
  if (strncmp(r->line, "Range ", 6) == 0) {
    bool ok = false;
    if (r->has_presence && r->line_size != 6) {
      uint32_t d = 0;
      ok = true;
      for (size_t i = 6; i < r->line_size; i++) {
        char c = r->line[i];
        if (c < '0' || c > '9') { ok = false; break; }
        d = d * 10U + (uint32_t)(c - '0');
        if (d > 65535U) { ok = false; break; }
      }
      if (ok) {
        *detectado = r->pending_presence;
        *distancia = (uint16_t)d;
      }
    }
    r->has_presence = false;
    limpiar_linea(r);
    if (ok) { r->valid_frames++; return true; }
    r->rejected_frames++;
    return false;
  }
  rechazar_linea(r);
  return false;
}

static bool push_byte(ld2420_t* r, uint8_t value, bool* detectado, uint16_t* distancia) {
  if (value == '\r') return false;
  if (value == '\n') return terminar_linea(r, detectado, distancia);
  if (r->overflowed) return false;
  if (r->line_size >= sizeof(r->line) - 1U) { r->overflowed = true; return false; }
  r->line[r->line_size++] = (char)value;
  r->line[r->line_size] = '\0';
  return false;
}

// ===== Detector de movimiento (NativeMotionDetector) =====
static void detector_add_sample(ld2420_t* r, uint32_t ahora, bool detectado) {
  r->accepted_samples++;
  r->last_sample_ms = ahora;
  if (!detectado) {
    if (!r->active) r->evidence = 0;
    return;
  }
  if (r->active) {
    r->last_positive_ms = ahora;
    return;
  }
  if (r->evidence == 0 ||
      (ahora - r->evidence_window_started_ms) > r->evidence_window_ms) {
    r->evidence = 1;
    r->evidence_window_started_ms = ahora;
  } else if (r->evidence < r->required_evidence) {
    r->evidence++;
  }
  if (r->evidence >= r->required_evidence) {
    r->active = true;
    r->last_positive_ms = ahora;
  }
}

static void detector_tick(ld2420_t* r, uint32_t ahora) {
  if (r->active && (ahora - r->last_positive_ms) > r->hold_ms) {
    r->active = false;
    r->evidence = 0;
  }
  if (!r->active && r->evidence > 0 &&
      (ahora - r->evidence_window_started_ms) > r->evidence_window_ms) {
    r->evidence = 0;
  }
}

bool ld2420_init(ld2420_t* r, int rx_pin, int tx_pin, uint32_t baud) {
  memset(r, 0, sizeof(*r));
  r->uart = UART_NUM_1;
  r->required_evidence = 2;
  r->evidence_window_ms = 500;
  r->hold_ms = 3000;
  r->stale_after_ms = 1000;
  r->stale = true;
  r->started = false;
  r->last_error = LD2420_ERR_NONE;

  uart_config_t cfg = {
      .baud_rate = (int)baud,
      .data_bits = UART_DATA_8_BITS,
      .parity    = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  if (uart_driver_install(r->uart, LD2420_UART_BUF, 0, 0, NULL, 0) != ESP_OK) {
    r->last_error = LD2420_ERR_NOT_STARTED;
    return false;
  }
  if (uart_param_config(r->uart, &cfg) != ESP_OK) {
    r->last_error = LD2420_ERR_NOT_STARTED;
    return false;
  }
  if (uart_set_pin(r->uart, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    r->last_error = LD2420_ERR_NOT_STARTED;
    return false;
  }

  configurar_modo_nativo(r);
  r->started = true;
  r->last_error = LD2420_ERR_NONE;
  return true;
}

void ld2420_update(ld2420_t* r) {
  if (!r->started) { r->last_error = LD2420_ERR_NOT_STARTED; return; }
  uint32_t ahora = aura_millis();

  uint8_t buf[64];
  int n;
  while ((n = uart_read_bytes(r->uart, buf, sizeof(buf), 0)) > 0) {
    for (int i = 0; i < n; i++) {
      bool detectado = false;
      uint16_t distancia = 0;
      if (push_byte(r, buf[i], &detectado, &distancia)) {
        detector_add_sample(r, ahora, detectado);
        r->sensor_detected = detectado;
        r->sensor_distance_cm = distancia;
      }
    }
  }
  detector_tick(r, ahora);

  uint32_t edad = r->accepted_samples == 0 ? 0 : (ahora - r->last_sample_ms);
  r->data_age_ms = edad;
  r->stale = r->accepted_samples == 0 || edad > r->stale_after_ms;
  if (r->stale) { r->active = false; r->evidence = 0; }
  r->motion = r->active;
  r->last_error = LD2420_ERR_NONE;
}

bool     ld2420_is_stale(const ld2420_t* r)          { return r->stale; }
bool     ld2420_movimiento(const ld2420_t* r)        { return r->motion; }
bool     ld2420_sensor_detecto(const ld2420_t* r)    { return r->sensor_detected; }
uint16_t ld2420_distancia_cm(const ld2420_t* r)      { return r->sensor_distance_cm; }
uint32_t ld2420_frames_validos(const ld2420_t* r)    { return r->valid_frames; }
uint32_t ld2420_frames_rechazados(const ld2420_t* r) { return r->rejected_frames; }
int      ld2420_ultimo_error(const ld2420_t* r)      { return r->last_error; }
