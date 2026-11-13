#include "pcd_rc522.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pines.h"
#include "../../../../comun/iso14443a.h"

static const char *TAG = "PCD";

/* Registros (hoja de datos del MFRC522) */
enum {
    REG_COMMAND = 0x01, REG_COMIRQ = 0x04, REG_ERROR = 0x06, REG_FIFO_DATA = 0x09, REG_FIFO_LEVEL = 0x0A,
    REG_CONTROL = 0x0C, REG_BIT_FRAMING = 0x0D, REG_COLL = 0x0E, REG_MODE = 0x11, REG_TX_MODE = 0x12,
    REG_RX_MODE = 0x13, REG_TX_CONTROL = 0x14, REG_TX_ASK = 0x15, REG_T_MODE = 0x2A, REG_T_PRESCALER = 0x2B,
    REG_T_RELOAD_H = 0x2C, REG_T_RELOAD_L = 0x2D, REG_MOD_WIDTH = 0x24, REG_VERSION = 0x37
};
enum { CMD_IDLE = 0x00, CMD_TRANSCEIVE = 0x0C, CMD_SOFT_RESET = 0x0F };

#define SPI_HOST SPI2_HOST
#define SPI_HZ 4000000
#define ESPERA_TRAMA_US 60000   /* el temporizador del chip corta a los ~25 ms; esto es el seguro */
#define FIFO_PEDAZO 32

static spi_device_handle_t disp;
static bool bus_listo;

static void escribir(uint8_t reg, uint8_t valor)
{
    uint8_t tx[2] = {(uint8_t)((reg << 1) & 0x7E), valor};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx};
    spi_device_polling_transmit(disp, &t);
}

static uint8_t leer(uint8_t reg)
{
    uint8_t tx[2] = {(uint8_t)(((reg << 1) & 0x7E) | 0x80), 0}, rx[2] = {0, 0};
    spi_transaction_t t = {.length = 16, .tx_buffer = tx, .rx_buffer = rx};
    spi_device_polling_transmit(disp, &t);
    return rx[1];
}

static void bits_arriba(uint8_t reg, uint8_t mascara) { escribir(reg, leer(reg) | mascara); }
static void bits_abajo(uint8_t reg, uint8_t mascara) { escribir(reg, leer(reg) & (uint8_t)~mascara); }

static void fifo_escribir(const uint8_t *d, size_t n)
{
    uint8_t tx[1 + FIFO_PEDAZO];
    while (n > 0) {
        size_t k = n > FIFO_PEDAZO ? FIFO_PEDAZO : n;
        tx[0] = (uint8_t)((REG_FIFO_DATA << 1) & 0x7E);
        memcpy(&tx[1], d, k);
        spi_transaction_t t = {.length = 8 * (1 + k), .tx_buffer = tx};
        spi_device_polling_transmit(disp, &t);
        d += k;
        n -= k;
    }
}

static void fifo_leer(uint8_t *d, size_t n)
{
    uint8_t tx[1 + FIFO_PEDAZO], rx[1 + FIFO_PEDAZO];
    while (n > 0) {
        size_t k = n > FIFO_PEDAZO ? FIFO_PEDAZO : n;
        memset(tx, (uint8_t)(((REG_FIFO_DATA << 1) & 0x7E) | 0x80), k);
        tx[k] = 0;
        spi_transaction_t t = {.length = 8 * (k + 1), .tx_buffer = tx, .rx_buffer = rx};
        spi_device_polling_transmit(disp, &t);
        memcpy(d, &rx[1], k);
        d += k;
        n -= k;
    }
}

esp_err_t pcd_bus_iniciar(void)
{
    if (bus_listo) return ESP_OK;
    spi_bus_config_t bus = {
        .miso_io_num = PIN_RC522_MISO,
        .mosi_io_num = PIN_RC522_MOSI,
        .sclk_io_num = PIN_RC522_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t e = spi_bus_initialize(SPI_HOST, &bus, SPI_DMA_DISABLED);
    if (e != ESP_OK) return e;
    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_RC522_SS,
        .queue_size = 1,
    };
    e = spi_bus_add_device(SPI_HOST, &dev, &disp);
    if (e != ESP_OK) return e;
    gpio_config_t rst = {.pin_bit_mask = 1ULL << PIN_RC522_RST, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&rst);
    gpio_set_level(PIN_RC522_RST, 1);
    bus_listo = true;
    return ESP_OK;
}

bool pcd_vivo(uint8_t *version, uint8_t *tx_control)
{
    uint8_t v = leer(REG_VERSION), tx = leer(REG_TX_CONTROL);
    if (version) *version = v;
    if (tx_control) *tx_control = tx;
    return (v == 0x91 || v == 0x92) && (tx & 0x03) == 0x03;
}

esp_err_t pcd_reiniciar(void)
{
    if (!bus_listo) return ESP_ERR_INVALID_STATE;
    gpio_set_level(PIN_RC522_RST, 0);          /* reinicio por hardware */
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RC522_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    escribir(REG_COMMAND, CMD_SOFT_RESET);     /* y por software, por las dudas */
    for (int i = 0; i < 50 && (leer(REG_COMMAND) & 0x10); i++) vTaskDelay(pdMS_TO_TICKS(2));

    escribir(REG_TX_MODE, 0x00);               /* CRC por nuestra cuenta: TxCRC y RxCRC apagados */
    escribir(REG_RX_MODE, 0x00);
    escribir(REG_MOD_WIDTH, 0x26);
    escribir(REG_T_MODE, 0x80);                /* temporizador automatico */
    escribir(REG_T_PRESCALER, 0xA9);
    escribir(REG_T_RELOAD_H, 0x03);
    escribir(REG_T_RELOAD_L, 0xE8);            /* ~25 ms sin respuesta */
    escribir(REG_TX_ASK, 0x40);
    escribir(REG_MODE, 0x3D);
    bits_arriba(REG_TX_CONTROL, 0x03);         /* antena */

    uint8_t v, tx;
    bool ok = pcd_vivo(&v, &tx);
    if (!ok) ESP_LOGW(TAG, "RC522 no responde bien (version=0x%02X TxControl=0x%02X)", v, tx);
    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void pcd_reiniciar_campo(void)
{
    bits_abajo(REG_TX_CONTROL, 0x03);
    vTaskDelay(pdMS_TO_TICKS(60));
    bits_arriba(REG_TX_CONTROL, 0x03);
    vTaskDelay(pdMS_TO_TICKS(10));
}

int pcd_txrx(void *ctx, const uint8_t *tx, size_t ntx, uint8_t tx_bits, uint8_t *rx, size_t max)
{
    (void)ctx;
    if (ntx == 0 || ntx > 64) return -1;
    escribir(REG_COMMAND, CMD_IDLE);
    escribir(REG_COMIRQ, 0x7F);
    escribir(REG_FIFO_LEVEL, 0x80);            /* vacia la FIFO */
    fifo_escribir(tx, ntx);
    escribir(REG_BIT_FRAMING, tx_bits & 0x07);
    bits_abajo(REG_COLL, 0x80);                /* despues de una colision, los bits se descartan */
    escribir(REG_COMMAND, CMD_TRANSCEIVE);
    bits_arriba(REG_BIT_FRAMING, 0x80);        /* StartSend */

    int64_t t0 = esp_timer_get_time();
    for (;;) {
        uint8_t irq = leer(REG_COMIRQ);
        if (irq & 0x30) break;                 /* RxIRq o IdleIRq: termino */
        if (irq & 0x01) return -1;             /* TimerIRq: la tarjeta no contesto */
        if ((esp_timer_get_time() - t0) > ESPERA_TRAMA_US) return -1;
        esp_rom_delay_us(60);
    }
    uint8_t err = leer(REG_ERROR);
    if (err & 0x1B) return -1;                 /* BufferOvfl, CollErr, ParityErr, ProtocolErr */
    size_t n = leer(REG_FIFO_LEVEL);
    if (n == 0 || n > max) return -1;
    fifo_leer(rx, n);
    return (int)n;
}

int pcd_trama_iso(void *ctx, const uint8_t *in, size_t n, uint8_t *out, size_t max)
{
    uint8_t tx[64], rx[64];
    if (n > sizeof(tx) - 2) return -1;
    memcpy(tx, in, n);
    iso14443a_crc(tx, n, &tx[n]);
    int r = pcd_txrx(ctx, tx, n + 2, 0, rx, sizeof(rx));
    if (r < 3 || !iso14443a_crc_ok(rx, (size_t)r)) return -1;
    size_t util = (size_t)r - 2;
    if (util > max) return -1;
    memcpy(out, rx, util);
    return (int)util;
}
