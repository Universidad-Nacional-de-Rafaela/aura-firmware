/* Tests de host del protocolo Pi -> hoja. Corre en la PC: cd tests && make */
#include <assert.h>
#include <stdio.h>
#include "protocolo_pi.h"

static linea_pi_t parsear_ok(const char *s)
{
    linea_pi_t l = {0};
    assert(protocolo_pi_parsear(s, &l));
    return l;
}

int main(void)
{
    linea_pi_t l;

    l = parsear_ok("P1 72");
    assert(l.confiable && l.presencia && l.conf_pct == 72);
    l = parsear_ok("P0 100");
    assert(l.confiable && !l.presencia && l.conf_pct == 100);
    l = parsear_ok("P1");
    assert(l.confiable && l.presencia && l.conf_pct == -1);
    l = parsear_ok("P0 0");
    assert(l.confiable && !l.presencia && l.conf_pct == 0);
    l = parsear_ok("U");
    assert(!l.confiable && l.conf_pct == -1);

    const char *invalidas[] = {"", "P", "P2", "P1 ", "P1 101", "P1 7a", "P1 1000", "P1  5",
                               "p1", "U1", "P10", "X", "P1 -5", "P1 72 extra"};
    for (size_t i = 0; i < sizeof invalidas / sizeof invalidas[0]; i++) {
        linea_pi_t basura = {0};
        assert(!protocolo_pi_parsear(invalidas[i], &basura));
    }
    assert(!protocolo_pi_parsear(NULL, &l));

    printf("test_protocolo: OK\n");
    return 0;
}
