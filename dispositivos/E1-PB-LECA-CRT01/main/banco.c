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
        "  tope [seg]          muestra cambios del microswitch (GPIO10) durante seg s\r\n"
        "  en 0|1              ENABLE del TB67S109 (1 = motor energizado)\r\n"
        "  dir 0|1             direccion (0 = hacia el tope, 1 = abrir)\r\n"
        "  paso N [us]         N pasos en la direccion actual, us = semiperiodo (def. 500)\r\n"
        "  home                homing: busca el microswitch de 0 %%\r\n"
        "  tot N               fija el recorrido total en pasos\r\n"
        "  ir N                va al N por ciento (requiere homing previo)\r\n"
        "  parar               aborta el movimiento o el homing\r\n"
        "  est                 estado: posicion, pasos, calibrada, tope, modo, luz\r\n"
        "  modo [0|1|2|manual|auto1|auto2]  cambia el modo de control\r\n"
        "  umbrales [bajo alto [conf_s]]    muestra o ajusta umbrales en lux y confirmacion\r\n"
        "  pines [S D E [T]]   muestra o cambia los GPIO de STEP, DIR, ENABLE y tope\r\n");
}

static void estado(void)
{
    static const char *nombres[] = {"detenida", "moviendo", "calibrando"};
    static const char *modos[] = {"manual", "auto1", "auto2"};
    double b, a, lx;
    int cf;
    bool ok;
    banco_obtener_umbrales(&b, &a, &cf, &ok, &lx);
    int m = banco_obtener_modo();
    const char *m_str = (m >= 0 && m <= 2) ? modos[m] : "?";
    bprintf("estado=%s calibrada=%d pos=%d%% pasos=%d/%d objetivo=%d%% tope=%d modo=%s luz=%.1flx(ok=%d) error=%s\r\n",
            nombres[cortina_estado()], cortina_calibrada(), cortina_pos_pct(), cortina_pasos_actuales(),
            cortina_pasos_totales(), cortina_objetivo_pct(), cortina_tope(), m_str, lx, ok,
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

static void cmd_tope(int seg)
{
    int ultimo = -1;
    bprintf("Apreta y solta el microswitch (GPIO10)... (0 = ACTIVADO, 1 = libre)\r\n");
    for (int t = 0; t < seg * 50; t++) {
        int h = cortina_tope();
        if (h != ultimo) {
            bprintf("  t=%.1fs  tope=%d  %s\r\n", t / 50.0, h, h ? "libre" : "ACTIVADO");
            ultimo = h;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void banco_correr(void)
{
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
        else if (!strcmp(cmd, "tope") || !strcmp(cmd, "hall")) cmd_tope(a1 ? atoi(a1) : 15);
        else if (!strcmp(cmd, "en") && a1) { cortina_banco_habilitar(atoi(a1)); bprintf("ENABLE %s\r\n", atoi(a1) ? "ON (energizado)" : "OFF"); }
        else if (!strcmp(cmd, "dir") && a1) { cortina_banco_direccion(atoi(a1)); bprintf("DIR=%d\r\n", atoi(a1) ? 1 : 0); }
        else if (!strcmp(cmd, "paso") && a1) {
            int n = atoi(a1); int us = a2 ? atoi(a2) : 500;
            if (n < 1 || n > 20000 || us < 200 || us > 20000) { bprintf("rango: n 1..20000, us 200..20000\r\n"); continue; }
            bprintf("%d pasos a %d us...\r\n", n, us);
            cortina_banco_pasos(n, (uint32_t)us);
            bprintf("listo\r\n");
        }
        else if (!strcmp(cmd, "home") || !strcmp(cmd, "cal")) { cortina_homing(); bprintf("homing iniciado; usar 'est' para seguir el avance\r\n"); }
        else if (!strcmp(cmd, "tot") && a1) { cortina_fijar_pasos_totales(atoi(a1)); estado(); }
        else if (!strcmp(cmd, "ir") && a1) { bprintf(cortina_mover_pct(atoi(a1)) ? "ok\r\n" : "rechazado: hacer 'home' primero\r\n"); }
        else if (!strcmp(cmd, "parar")) { cortina_parar(); bprintf("parando\r\n"); }
        else if (!strcmp(cmd, "est")) estado();
        else if (!strcmp(cmd, "modo")) {
            if (a1) {
                if (!strcmp(a1, "0") || !strcmp(a1, "manual")) banco_establecer_modo(0);
                else if (!strcmp(a1, "1") || !strcmp(a1, "auto1")) banco_establecer_modo(1);
                else if (!strcmp(a1, "2") || !strcmp(a1, "auto2")) banco_establecer_modo(2);
                else { bprintf("usar: modo 0|1|2|manual|auto1|auto2\r\n"); continue; }
            }
            estado();
        }
        else if (!strcmp(cmd, "umbrales")) {
            char *a3 = strtok(NULL, " ");
            double b, a, lx; int cf; bool ok;
            banco_obtener_umbrales(&b, &a, &cf, &ok, &lx);
            if (a1 && a2) {
                double nb = atof(a1), na = atof(a2);
                int ncf = a3 ? atoi(a3) : cf;
                if (nb < 0 || nb >= na || na > 54612 || ncf < 0 || ncf > 600) { bprintf("rango invalido (0 <= bajo < alto <= 54612, conf 0..600)\r\n"); continue; }
                banco_establecer_umbrales(nb, na, ncf);
            }
            banco_obtener_umbrales(&b, &a, &cf, &ok, &lx);
            bprintf("umbrales: bajo=%.1f lx, alto=%.1f lx, confirmacion=%d s\r\n", b, a, cf);
        }
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
