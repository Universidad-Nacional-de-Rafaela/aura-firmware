#pragma once
// Utilidades comunes a los modulos portados: milisegundos monotonicos y el
// tag de log. En Arduino esto era millis() y Serial; en ESP-IDF puro es el
// esp_timer y ESP_LOG.
#include <stdint.h>
#include "esp_timer.h"
#include "esp_log.h"

// Milisegundos desde el arranque, con el mismo desbordamiento a 32 bits que
// millis(): las comparaciones "ahora - antes" siguen funcionando por aritmetica
// modular.
static inline uint32_t aura_millis(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

#define AURA_TAG "aura"
