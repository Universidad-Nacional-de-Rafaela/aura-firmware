#include "lector.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pcd_rc522.h"
#include "../../../../comun/iso14443a.h"
#include "../../../../comun/iso_dep.h"

static const char *TAG = "LECTOR";

#define PERIODO_ESPERA_MS 80
#define PERIODO_PRESENTE_MS 150
#define AUSENCIAS_PARA_RETIRADA 3

typedef enum { ESPERA, PRESENTE } estado_t;

static bool arrancado;                  /* el RC522 contesto y quedo configurado alguna vez */
static estado_t estado = ESPERA;
static int ausencias;
static uint8_t uid_presente[ISO14443A_UID_MAX];
static uint8_t largo_presente;
static QueueHandle_t cola_lecturas;
static SemaphoreHandle_t mutex_lector;
static lector_verificador_t verificador;

/* ISO-DEP de la tarjeta que se esta atendiendo */
static isodep_t dep;
static bool dep_listo;

void lector_set_verificador(lector_verificador_t v) { verificador = v; }

static const iso14443a_ops_t ops_aire = {.txrx = pcd_txrx, .ctx = NULL};

static int apdu_tarjeta(void *ctx, const uint8_t *cmd, size_t n, uint8_t *resp, size_t max)
{
    (void)ctx;
    if (!dep_listo) {
        dep = (isodep_t){.trama = pcd_trama_iso, .ctx = NULL};
        if (!isodep_rats(&dep)) return -1;
        dep_listo = true;
    }
    return isodep_apdu(&dep, cmd, n, resp, max);
}

/* Saca a la tarjeta del estado ISO-DEP (apaga el campo: vuelve a IDLE) y la deja en HALT, para que
   no conteste los REQA y se la pueda seguir viendo con WUPA sin que se lea otra vez. */
static void dormir_tarjeta(void)
{
    pcd_reiniciar_campo();
    uint8_t atqa[2];
    iso14443a_tarjeta_t t;
    if (iso14443a_despertar(&ops_aire, false, atqa) == ISO14443A_OK &&
        iso14443a_seleccionar(&ops_aire, &t) == ISO14443A_OK) {
        iso14443a_halt(&ops_aire);
    }
}

static void atender_nueva(const iso14443a_tarjeta_t *t)
{
    lectura_t l = {.largo = t->largo, .auth = LECTOR_AUTH_SIN_VERIFICADOR};
    if (t->largo > UID_MAX_BYTES) return;
    memcpy(l.uid, t->uid, t->largo);
    dep_listo = false;
    if (verificador) l.auth = verificador(l.uid, l.largo, apdu_tarjeta, NULL);
    xQueueSend(cola_lecturas, &l, 0);
    memcpy(uid_presente, t->uid, t->largo);
    largo_presente = t->largo;
    dormir_tarjeta();
    estado = PRESENTE;
    ausencias = 0;
}

static void paso(void)
{
    uint8_t atqa[2];
    iso14443a_tarjeta_t t;
    bool wupa = (estado == PRESENTE);
    iso14443a_resultado_t r = iso14443a_despertar(&ops_aire, wupa, atqa);
    if (r == ISO14443A_SIN_TARJETA) {
        if (estado == PRESENTE && ++ausencias >= AUSENCIAS_PARA_RETIRADA) {
            estado = ESPERA;
            ESP_LOGD(TAG, "Tarjeta retirada");
        }
        return;
    }
    if (r != ISO14443A_OK || iso14443a_seleccionar(&ops_aire, &t) != ISO14443A_OK) {
        pcd_reiniciar_campo();          /* una tarjeta a medio seleccionar: que arranque de cero */
        if (estado == PRESENTE) ausencias++;
        return;
    }
    if (estado == PRESENTE && t.largo == largo_presente && memcmp(t.uid, uid_presente, t.largo) == 0) {
        iso14443a_halt(&ops_aire);      /* sigue ahi: se la vuelve a dormir */
        ausencias = 0;
        return;
    }
    atender_nueva(&t);                  /* tarjeta nueva (o cambiada de golpe) */
}

static void tarea_lector(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(mutex_lector, portMAX_DELAY);
        if (arrancado) paso();
        estado_t e = estado;
        xSemaphoreGive(mutex_lector);
        vTaskDelay(pdMS_TO_TICKS(e == PRESENTE ? PERIODO_PRESENTE_MS : PERIODO_ESPERA_MS));
    }
}

esp_err_t lector_iniciar(QueueHandle_t cola)
{
    cola_lecturas = cola;
    mutex_lector = xSemaphoreCreateMutex();
    esp_err_t e = pcd_bus_iniciar();
    if (e != ESP_OK) return e;
    e = pcd_reiniciar();
    arrancado = (e == ESP_OK);
    /* la tarea existe siempre: si el RC522 no estaba al encender, lector_revisar() lo levanta */
    if (xTaskCreate(tarea_lector, "lector", 4096, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return e;
}

lector_salud_t lector_revisar(void)
{
    if (xSemaphoreTake(mutex_lector, pdMS_TO_TICKS(2500)) != pdTRUE) {
        return arrancado ? LECTOR_OK : LECTOR_CAIDO;       /* ocupado atendiendo una tarjeta */
    }
    uint8_t version = 0, antena = 0;
    bool registros_bien = pcd_vivo(&version, &antena);
    lector_paso_t p = lector_proximo_paso(true, arrancado, registros_bien);
    bool salio_bien = false;
    if (p == LECTOR_PASO_ARRANCAR || p == LECTOR_PASO_REINICIAR) {
        ESP_LOGW(TAG, "El RC522 no esta bien (version=0x%02X antena=0x%02X): lo reinicio", version, antena);
        salio_bien = pcd_reiniciar() == ESP_OK;
        arrancado = salio_bien;
        if (salio_bien) estado = ESPERA;
    }
    xSemaphoreGive(mutex_lector);
    return lector_salud_tras(p, salio_bien);
}
