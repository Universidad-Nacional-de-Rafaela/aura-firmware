/*
 * Puerta con llavero de proximidad (modelo PUE-RFID) en MODO INDEPENDIENTE, sin conexion a AURA.
 * Reemplaza a main.c cuando se compila con CONFIG_PUERTA_INDEPENDIENTE (ADR-009, "Etapa
 * transitoria: puertas sin conexion"). Ficha: README.md del modelo.
 *
 * No inicia el WiFi ni la mesh, no usa la NVS y no incluye config_local.h: la imagen no lleva la
 * clave de la mesh y de la placa no sale ningun dato. La lista de llaveros se compila desde
 * llaveros/<CONFIG_PUERTA_CODIGO>.h, que no se versiona. Los UID no se imprimen.
 *
 *   independiente_logica.h          decide (sobre puerta_logica.h; probada en la compu)
 *   este archivo                    cablea: lee el sensor y el lector y ejecuta lo decidido
 *   rele.c, sonido.c y lector.c     los mismos que en la puerta conectada
 */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "independiente_logica.h"
#include "lector.h"
#include "pines.h"
#include "rele.h"
#include "sensor_logica.h"
#include "sonido.h"

/* La lista de esta puerta. La ruta la arma main/CMakeLists.txt con CONFIG_PUERTA_CODIGO. */
#if __has_include(PUERTA_LLAVEROS_H)
#include PUERTA_LLAVEROS_H
#else
#error "Falta la lista de llaveros: poné el código de la puerta en CONFIG_PUERTA_CODIGO y copiá llaveros/llaveros.ejemplo.h como llaveros/<código>.h"
#endif

#define LLAVEROS_CANTIDAD (sizeof(LLAVEROS) / sizeof(LLAVEROS[0]))
_Static_assert(LLAVEROS_CANTIDAD >= 1 && LLAVEROS_CANTIDAD <= LISTA_MAX,
               "El archivo de llaveros tiene que tener entre 1 y 32 UID");

static const char *TAG = "PUERTA";

#if CONFIG_PUERTA_SENSOR_BOTON
#define SENSOR_TIPO SENSOR_BOTON
#else
#define SENSOR_TIPO SENSOR_REED
#endif

#define PERIODO_LOGICA_MS 10
#define PERIODO_LECTOR_MS 5000
#define LECTOR_CAIDO_REINICIO_MS (5 * 60 * 1000)   /* un lector que no vuelve en 5 min: se reinicia la placa */

static lista_t lista;                      /* se carga antes de crear la tarea y no cambia mas */
static QueueHandle_t cola_lecturas;
static puerta_t puerta;                    /* solo la toca la tarea de la logica */

static inline int64_t ahora_ms(void) { return esp_timer_get_time() / 1000; }

static void ejecutar_acciones(void)
{
    accion_t a;
    while (independiente_sacar(&puerta, &a)) {
        switch (a.tipo) {
        case ACC_PULSO:
            rele_energizar((uint32_t)a.valor);
            ESP_LOGI(TAG, "Cerradura destrabada %d ms", (int)a.valor);
            break;
        case ACC_CORTAR_PULSO:
            rele_soltar();
            ESP_LOGI(TAG, "Cerradura trabada");
            break;
        case ACC_SONIDO:
            sonido_puntual((sonido_t)a.valor);
            break;
        case ACC_AVISO:
            sonido_aviso_fijar((aviso_t)a.valor, a.desde_ms, a.hasta_ms);
            break;
        default:                           /* independiente_sacar no entrega las demas */
            break;
        }
    }
}

static void tarea_puerta(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    /* Para comprobar al instalar que se grabo la lista de esta puerta: codigo, cantidad y hash */
    char hash[HASH_HEX_BYTES];
    lista_hash(&lista, hash);
    ESP_LOGI(TAG, "Modo independiente (sin conexion). Lista %s: %d llaveros, hash %s", CONFIG_PUERTA_CODIGO,
             lista.n, hash);

    vTaskDelay(pdMS_TO_TICKS(100));        /* que la pull-up del sensor se estabilice */
    sensor_filtro_t sensor;
    sensor_filtro_iniciar(&sensor, SENSOR_TIPO, gpio_get_level(PIN_SENSOR), ahora_ms());
    independiente_iniciar(&puerta, ahora_ms(), sensor.estable);
    ESP_LOGI(TAG, "Arranca con la puerta %s", sensor.estable ? "abierta" : "cerrada");

    int64_t proximo_lector = ahora_ms() + PERIODO_LECTOR_MS, lector_caido_desde = 0;

    while (true) {
        esp_task_wdt_reset();
        int64_t ahora = ahora_ms();

        /* 1. El sensor */
        bool abierta;
        if (sensor_filtro_paso(&sensor, ahora, gpio_get_level(PIN_SENSOR), &abierta) == SENSOR_CAMBIO) {
            ESP_LOGI(TAG, "Puerta %s", abierta ? "ABIERTA" : "cerrada");
            puerta_sensor(&puerta, ahora, abierta);
        }
        ejecutar_acciones();

        /* 2. Llaveros */
        lectura_t l;
        while (xQueueReceive(cola_lecturas, &l, 0) == pdTRUE) {
            bool autorizada = lista_autorizada(&lista, l.uid, l.largo);
            ESP_LOGI(TAG, "Llavero %s", autorizada ? "autorizado" : "RECHAZADO");
            independiente_tarjeta(&puerta, ahora, l.uid, l.largo, autorizada);
            ejecutar_acciones();
        }

        /* 3. El paso del tiempo: vence el pulso, los avisos, los topes */
        puerta_tick(&puerta, ahora);
        ejecutar_acciones();

        /* 4. Salud del lector, cada tanto */
        if ((ahora - proximo_lector) >= 0) {
            proximo_lector = ahora + PERIODO_LECTOR_MS;
            if (lector_revisar() == LECTOR_CAIDO) {
                if (lector_caido_desde == 0) lector_caido_desde = ahora;
                if ((ahora - lector_caido_desde) >= LECTOR_CAIDO_REINICIO_MS) {
                    ESP_LOGE(TAG, "El lector no vuelve: reinicio la placa (queda trabada)");
                    esp_restart();
                }
            } else {
                lector_caido_desde = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PERIODO_LOGICA_MS));
    }
}

void app_main(void)
{
    /* LO PRIMERO: la cerradura en reposo (trabada), antes que todo lo demas */
    rele_reposo_inicial();
    rele_iniciar();

    gpio_config_t entrada = {
        .pin_bit_mask = 1ULL << PIN_SENSOR,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&entrada));

    size_t primera_mala;
    size_t malas = independiente_cargar_lista(&lista, LLAVEROS, LLAVEROS_CANTIDAD, &primera_mala);
    if (malas > 0) {
        ESP_LOGE(TAG, "Entradas del archivo de llaveros que no sirven (mal escritas o repetidas): %u; la primera es la %u",
                 (unsigned)malas, (unsigned)primera_mala);
    }

    cola_lecturas = xQueueCreate(4, sizeof(lectura_t));
    sonido_iniciar();
    esp_err_t err = lector_iniciar(cola_lecturas);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "El lector RC522 no arranca (%s): sin lector no se abre con llavero", esp_err_to_name(err));
    }

    xTaskCreate(tarea_puerta, "puerta", 6144, NULL, 5, NULL);
}
