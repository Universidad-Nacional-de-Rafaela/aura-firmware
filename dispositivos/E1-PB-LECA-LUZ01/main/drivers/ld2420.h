#pragma once
// LD2420 (radar de presencia) modo nativo "simple" para ESP-IDF puro.
// Puerto de la libreria LD2420HumanMotion (modo NativeSimple): el radar manda
// lineas de texto "ON", "OFF" y "Range <cm>\n"; el detector exige 2 votos en
// una ventana de 500 ms, mantiene 3 s (hold) y marca stale a 1 s sin datos.
#include <stdint.h>
#include <stdbool.h>
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uart_port_t uart;

  // Parser de lineas nativas.
  char     line[24];
  uint32_t line_size;
  bool     overflowed;
  bool     has_presence;
  bool     pending_presence;
  uint32_t valid_frames;
  uint32_t rejected_frames;

  // Detector de movimiento (NativeMotionDetector).
  uint8_t  required_evidence;
  uint32_t evidence_window_ms;
  uint32_t hold_ms;
  uint32_t stale_after_ms;
  uint8_t  evidence;
  uint32_t accepted_samples;
  uint32_t last_sample_ms;
  uint32_t evidence_window_started_ms;
  uint32_t last_positive_ms;
  bool     active;

  // Estado reflejado.
  bool     sensor_detected;
  uint16_t sensor_distance_cm;
  bool     started;
  bool     stale;
  bool     motion;
  uint32_t data_age_ms;
  int      last_error;
} ld2420_t;

enum {
  LD2420_ERR_NONE = 0,
  LD2420_ERR_NOT_STARTED,
  LD2420_ERR_TIMEOUT,
  LD2420_ERR_MALFORMED
};

// Configura el UART y manda la secuencia de "modo reporte nativo". false si el
// UART no arranca.
bool ld2420_init(ld2420_t* r, int rx_pin, int tx_pin, uint32_t baud);

// Lee lo que haya en el UART, actualiza el detector y el estado. Llamar seguido.
void ld2420_update(ld2420_t* r);

bool     ld2420_is_stale(const ld2420_t* r);
bool     ld2420_movimiento(const ld2420_t* r);
bool     ld2420_sensor_detecto(const ld2420_t* r);
uint16_t ld2420_distancia_cm(const ld2420_t* r);
uint32_t ld2420_frames_validos(const ld2420_t* r);
uint32_t ld2420_frames_rechazados(const ld2420_t* r);
int      ld2420_ultimo_error(const ld2420_t* r);

#ifdef __cplusplus
}
#endif
