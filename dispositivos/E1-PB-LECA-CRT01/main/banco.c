/*
 * Consola de banco por el USB de la placa (USB-Serial-JTAG). Se activa con
 * #define AURA_MODO_BANCO 1 en config_local.h. No usa la malla.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "cortina.h"
#include "luz.h"
#include "banco.h"

static void eco(const char *s, size_t n) { usb_serial_jtag_write_bytes(s, n, pdMS_TO_TICKS(50)); }

static void bprintf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
        eco(buf, len);
    }
}

static void leer_linea(char *buf, size_t max)
{
    size_t len = 0;
    bprintf("\r\nbanco> ");
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(50)) <= 0) continue;
        if (c == '\r' || c == '\n') {
            if (len == 0) continue;
            buf[len] = 0;
            eco("\r\n", 2);
            return;
        }
        if (c == 0x7F || c == 8) { if (len) { len--; eco("\b \b", 3); } continue; }
        if (len < max - 1) { buf[len++] = (char)c; eco((char *)&c, 1); }
    }
}

static void ayuda(void)
{
    bprintf(
        "Comandos:\r\n"
        "  i2c                 escanea el bus I2C (SDA=6, SCL=7)\r\n"
        "  lux [n]             lee el BH1750 n veces (1 por segundo)\r\n"
        "  hall [seg]          muestra cambios del tope (GPIO10) durante seg s\r\n"
        "  en 0|1              ENABLE del TB67S109 (1 = motor energizado)\r\n"
        "  dir 0|1             direccion (0 = hacia el tope, 1 = abrir)\r\n"
        "  paso N [us]         N pasos en la direccion actual, us = semiperiodo (def. 2000)\r\n"
        "  cal                 calibracion completa (busca el tope y mide el recorrido)\r\n"
        "  tot N               fija a mano el recorrido total en pasos\r\n"
        "  ir N                va al N por ciento (requiere cal o tot)\r\n"
        "  parar               aborta el movimiento o la calibracion\r\n"
        "  est                 estado: posicion, pasos, calibrada, hall, error\r\n"
        "  pines [S D E [H]]   muestra o cambia los GPIO de STEP, DIR, ENABLE y tope (ej.: pines 3 4 5 8)\r\n");
}

static void estado(void)
{
    static const char *nombres[] = {"detenida", "moviendo", "calibrando"};
    bprintf("estado=%s calibrada=%d pos=%d%% pasos=%d/%d objetivo=%d%% hall=%d error=%s\r\n",
            nombres[cortina_estado()], cortina_calibrada(), cortina_pos_pct(), cortina_pasos_actuales(),
            cortina_pasos_totales(), cortina_objetivo_pct(), cortina_hall(),
            cortina_error() ? cortina_error() : "-");
}

static void cmd_lux(int n)
{
    for (int i = 0; i < n; i++) {
        double lux;
        if (luz_leer(&lux)) bprintf("lux = %.1f\r\n", lux);
        else bprintf("sin dato (sensor no responde o calentando; probar 'i2c')\r\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void cmd_hall(int seg)
{
    int ultimo = -1;
    bprintf("Apreta y solta el switch (GPIO10)... (0 = ACTIVADO, 1 = libre)\r\n");
    for (int t = 0; t < seg * 50; t++) {
        int h = cortina_hall();
        if (h != ultimo) {
            bprintf("  t=%.1fs  hall=%d  %s\r\n", t / 50.0, h, h ? "libre" : "ACTIVADO");
            ultimo = h;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void banco_correr(void)
{
    cortina_iniciar(false, NULL);        /* sin calibrar al arrancar */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&cfg);
    bprintf("\r\n=== LUFLEX - firmware de banco ===\r\n");
    ayuda();
    char l[64];
    for (;;) {
        leer_linea(l, sizeof(l));
        char *cmd = strtok(l, " ");
        char *a1 = strtok(NULL, " ");
        char *a2 = strtok(NULL, " ");
        if (!cmd) continue;
        if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) ayuda();
        else if (!strcmp(cmd, "i2c")) luz_escanear();
        else if (!strcmp(cmd, "lux")) cmd_lux(a1 ? atoi(a1) : 5);
        else if (!strcmp(cmd, "hall")) cmd_hall(a1 ? atoi(a1) : 15);
        else if (!strcmp(cmd, "en") && a1) { cortina_banco_habilitar(atoi(a1)); bprintf("ENABLE %s\r\n", atoi(a1) ? "ON (energizado)" : "OFF"); }
        else if (!strcmp(cmd, "dir") && a1) { cortina_banco_direccion(atoi(a1)); bprintf("DIR=%d\r\n", atoi(a1) ? 1 : 0); }
        else if (!strcmp(cmd, "paso") && a1) {
            int n = atoi(a1); int us = a2 ? atoi(a2) : 2000;
            if (n < 1 || n > 20000 || us < 200 || us > 20000) { bprintf("rango: n 1..20000, us 200..20000\r\n"); continue; }
            bprintf("%d pasos a %d us...\r\n", n, us);
            cortina_banco_pasos(n, (uint32_t)us);
            bprintf("listo\r\n");
        }
        else if (!strcmp(cmd, "cal")) { cortina_calibrar(); bprintf("calibrando; usar 'est' para seguir el avance\r\n"); }
        else if (!strcmp(cmd, "tot") && a1) { cortina_banco_fijar_totales(atoi(a1)); estado(); }
        else if (!strcmp(cmd, "ir") && a1) { bprintf(cortina_mover_pct(atoi(a1)) ? "ok\r\n" : "rechazado: sin calibrar\r\n"); }
        else if (!strcmp(cmd, "parar")) { cortina_parar(); bprintf("parando\r\n"); }
        else if (!strcmp(cmd, "est")) estado();
        else if (!strcmp(cmd, "pines")) {
            char *a3 = strtok(NULL, " ");
            if (a1 && a2 && a3) {
                char *a4 = strtok(NULL, " ");
                int s = atoi(a1), d = atoi(a2), e = atoi(a3), h = a4 ? atoi(a4) : -1;
                int s0, d0, e0, h0;
                cortina_banco_pines_actuales(&s0, &d0, &e0, &h0);
                if (h < 0) h = h0;
                if (s < 0 || d < 0 || e < 0 || s > 10 || d > 10 || e > 10 || h > 10) { bprintf("GPIO validos: 0..10\r\n"); continue; }
                cortina_banco_pines(s, d, e, h);
            }
            int s, d, e, h;
            cortina_banco_pines_actuales(&s, &d, &e, &h);
            bprintf("pines: STEP=GPIO%d DIR=GPIO%d ENABLE=GPIO%d TOPE=GPIO%d\r\n", s, d, e, h);
        }
        else bprintf("comando desconocido; 'help' para la lista\r\n");
    }
}
