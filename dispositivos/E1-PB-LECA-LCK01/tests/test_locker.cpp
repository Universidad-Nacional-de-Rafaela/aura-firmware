// Logica del gabinete de 4 lockers: un solo pulso a la vez en el gabinete, limite por minuto por
// cerradura, cortar al abrir, alertas del contrato v4.5 con su puerta (intrusion,
// arranque_puerta_abierta, puerta_abierta, puerta_cerrada, limite_pulsos), puerta_invalida,
// antirrebote y vigencia de la orden de abrir.
#include "../main/locker_logica.h"
#include "../../../comun/tests/aserciones.h"
#include <cmath>
#include <string>
#include <vector>

// Saca toda la cola como texto: "PULSO 2 3000", "CORTAR 2", "ALERTA 2 intrusion critical", "MEDIR"
static std::vector<std::string> acciones(gabinete_t *g) {
  std::vector<std::string> v;
  accion_t a;
  char buf[80];
  while (gabinete_sacar(g, &a)) {
    switch (a.tipo) {
    case ACC_PULSO: snprintf(buf, sizeof(buf), "PULSO %d %d", a.puerta, (int)a.valor); break;
    case ACC_CORTAR: snprintf(buf, sizeof(buf), "CORTAR %d", a.puerta); break;
    case ACC_ALERTA: snprintf(buf, sizeof(buf), "ALERTA %d %s %s", a.puerta, a.alerta, severidad_texto(a.severidad)); break;
    case ACC_MEDIR: snprintf(buf, sizeof(buf), "MEDIR"); break;
    }
    v.push_back(buf);
  }
  return v;
}

static bool es(const std::vector<std::string> &v, std::initializer_list<const char *> e) {
  if (v.size() != e.size()) return false;
  size_t i = 0;
  for (const char *s : e) if (v[i++] != s) return false;
  return true;
}

static const bool CERRADOS[CANT_LOCKERS] = {false, false, false, false};

int main() {
  gabinete_t g;

  // Apertura normal del locker 2 sin cortar al abrir: pulso, abre, vence, cierra
  gabinete_iniciar(&g, 0, CERRADOS, 3000, 120, false);
  VERIFICAR(acciones(&g).empty());                             // arrancar cerrados: nada
  VERIFICAR(gabinete_abrir(&g, 1000, 2) == ABRIR_OK);
  VERIFICAR(es(acciones(&g), {"PULSO 2 3000", "MEDIR"}));
  gabinete_sensor(&g, 1, 2000, true);
  VERIFICAR(es(acciones(&g), {"MEDIR"}));                      // abierto con orden: sin alerta
  gabinete_tick(&g, 3999);
  VERIFICAR(acciones(&g).empty());
  gabinete_tick(&g, 4000);
  VERIFICAR(es(acciones(&g), {"CORTAR 2", "MEDIR"}));
  gabinete_sensor(&g, 1, 10000, false);
  VERIFICAR(es(acciones(&g), {"MEDIR"}));

  // puerta_invalida: 0, 5 y fuera de rango; sin puerta lo resuelve puerta_valida
  VERIFICAR(gabinete_abrir(&g, 20000, 0) == ABRIR_PUERTA_INVALIDA);
  VERIFICAR(gabinete_abrir(&g, 20000, 5) == ABRIR_PUERTA_INVALIDA);
  int p = -1;
  VERIFICAR(!puerta_valida(false, 0, &p));                    // no vino puerta
  VERIFICAR(!puerta_valida(true, 5, &p));
  VERIFICAR(!puerta_valida(true, 0, &p));
  VERIFICAR(!puerta_valida(true, 1.5, &p));
  VERIFICAR(!puerta_valida(true, std::nan(""), &p));
  VERIFICAR(p == -1);
  VERIFICAR(puerta_valida(true, 4, &p) && p == 4);
  VERIFICAR(acciones(&g).empty());

  // Un solo pulso a la vez en el gabinete: con el 1 energizado, abrir el 1 o el 3 da ocupada
  gabinete_iniciar(&g, 0, CERRADOS, 3000, 120, false);
  VERIFICAR(gabinete_abrir(&g, 0, 1) == ABRIR_OK);
  acciones(&g);
  VERIFICAR(gabinete_abrir(&g, 1000, 1) == ABRIR_OCUPADA);    // el mismo: no se extiende
  VERIFICAR(gabinete_abrir(&g, 1000, 3) == ABRIR_OCUPADA);    // otro: no se superpone
  VERIFICAR(acciones(&g).empty());
  VERIFICAR(g.l[0].cerradura.vence_ms == 3000);
  VERIFICAR(!g.l[2].cerradura.energizada);
  gabinete_tick(&g, 3000);
  acciones(&g);
  VERIFICAR(gabinete_abrir(&g, 3000, 3) == ABRIR_OK);         // terminado el pulso, ya se puede
  VERIFICAR(es(acciones(&g), {"PULSO 3 3000", "MEDIR"}));

  // Cortar al abrir: el pulso del locker 4 se corta apenas su sensor marca abierto
  gabinete_iniciar(&g, 0, CERRADOS, 3000, 120, true);
  gabinete_abrir(&g, 0, 4);
  acciones(&g);
  gabinete_sensor(&g, 3, 1200, true);
  VERIFICAR(es(acciones(&g), {"CORTAR 4", "MEDIR"}));
  VERIFICAR(!gabinete_ocupado(&g));
  VERIFICAR(gabinete_abrir(&g, 1300, 2) == ABRIR_OK);         // el gabinete ya esta libre

  // Intrusion en el locker 3 (sin pulso): la alerta, con su numero, sale ANTES que la medicion
  gabinete_iniciar(&g, 0, CERRADOS, 3000, 120, true);
  gabinete_sensor(&g, 2, 5000, true);
  VERIFICAR(es(acciones(&g), {"ALERTA 3 intrusion critical", "MEDIR"}));
  gabinete_sensor(&g, 2, 8000, false);
  VERIFICAR(es(acciones(&g), {"ALERTA 3 puerta_cerrada info", "MEDIR"}));

  // El pulso de un locker no habilita a otro: el 1 energizado, se abre el 2 -> intrusion del 2
  gabinete_iniciar(&g, 0, CERRADOS, 3000, 120, false);
  gabinete_abrir(&g, 0, 1);
  acciones(&g);
  gabinete_sensor(&g, 1, 500, true);
  VERIFICAR(es(acciones(&g), {"ALERTA 2 intrusion critical", "MEDIR"}));

  // Gracia: hasta 3 s despues del fin del pulso no es intrusion; despues si
  gabinete_iniciar(&g, 0, CERRADOS, 3000, 120, false);
  gabinete_abrir(&g, 0, 1);
  gabinete_tick(&g, 3000);
  acciones(&g);
  gabinete_sensor(&g, 0, 5999, true);
  VERIFICAR(es(acciones(&g), {"MEDIR"}));
  gabinete_sensor(&g, 0, 7000, false);
  acciones(&g);
  gabinete_sensor(&g, 0, 9000, true);                          // 6 s despues del fin del pulso
  VERIFICAR(es(acciones(&g), {"ALERTA 1 intrusion critical", "MEDIR"}));

  // Arrancar con los lockers 2 y 4 abiertos: arranque_puerta_abierta (high) de cada uno, sin
  // intrusion; al cerrarlos, puerta_cerrada
  const bool abiertos[CANT_LOCKERS] = {false, true, false, true};
  gabinete_iniciar(&g, 0, abiertos, 3000, 0, false);
  VERIFICAR(es(acciones(&g), {"ALERTA 2 arranque_puerta_abierta high",
                              "ALERTA 4 arranque_puerta_abierta high"}));
  gabinete_tick(&g, 1000);
  VERIFICAR(acciones(&g).empty());
  gabinete_sensor(&g, 1, 2000, false);
  VERIFICAR(es(acciones(&g), {"ALERTA 2 puerta_cerrada info", "MEDIR"}));

  // puerta_abierta a los aviso_s, una vez por apertura y por locker
  gabinete_iniciar(&g, 0, CERRADOS, 3000, 10, true);
  gabinete_abrir(&g, 0, 1);
  gabinete_sensor(&g, 0, 500, true);
  acciones(&g);
  gabinete_tick(&g, 10499);
  VERIFICAR(acciones(&g).empty());
  gabinete_tick(&g, 10500);
  VERIFICAR(es(acciones(&g), {"ALERTA 1 puerta_abierta warning"}));
  gabinete_tick(&g, 20000);
  VERIFICAR(acciones(&g).empty());
  gabinete_sensor(&g, 0, 30000, false);
  VERIFICAR(es(acciones(&g), {"ALERTA 1 puerta_cerrada info", "MEDIR"}));

  // aviso_s = 0: sin aviso
  gabinete_iniciar(&g, 0, CERRADOS, 3000, 0, true);
  gabinete_abrir(&g, 0, 1);
  gabinete_sensor(&g, 0, 500, true);
  acciones(&g);
  gabinete_tick(&g, 3600000);
  VERIFICAR(acciones(&g).empty());

  // Limite por minuto POR CERRADURA: el 1 llega a 20 s y da limite_pulsos; el 2 sigue pudiendo
  gabinete_iniciar(&g, 0, CERRADOS, 5000, 120, false);
  int64_t t = 0;
  for (int i = 0; i < 4; i++) {
    VERIFICAR(gabinete_abrir(&g, t, 1) == ABRIR_OK);
    t += 5000;
    gabinete_tick(&g, t);
  }
  acciones(&g);
  VERIFICAR(gabinete_abrir(&g, t, 1) == ABRIR_LIMITE);
  VERIFICAR(es(acciones(&g), {"ALERTA 1 limite_pulsos warning"}));
  VERIFICAR(!gabinete_ocupado(&g));
  VERIFICAR(gabinete_abrir(&g, t, 2) == ABRIR_OK);            // otra bobina, otro limite

  // Antirrebote: un rebote de 20 ms no cambia nada; 50 ms estables si
  antirrebote_t a;
  antirrebote_iniciar(&a, false, 0);
  VERIFICAR(!antirrebote_paso(&a, true, 10));
  VERIFICAR(!antirrebote_paso(&a, false, 30));
  VERIFICAR(!antirrebote_paso(&a, false, 200));
  VERIFICAR(!a.estable);
  VERIFICAR(!antirrebote_paso(&a, true, 300));
  VERIFICAR(!antirrebote_paso(&a, true, 349));
  VERIFICAR(antirrebote_paso(&a, true, 350));
  VERIFICAR(a.estable);

  // Vigencia de la orden de abrir
  const int64_t ahora = 1764000000;                           // nov 2025
  VERIFICAR(orden_vigente(false, 0, ahora) == ORDEN_SIN_VENCIMIENTO);
  VERIFICAR(orden_vigente(true, ahora + 60, 1000) == ORDEN_SIN_HORA);
  VERIFICAR(orden_vigente(true, ahora, ahora) == ORDEN_VENCIDA);
  VERIFICAR(orden_vigente(true, ahora + 301, ahora) == ORDEN_VENCIMIENTO_LEJANO);
  VERIFICAR(orden_vigente(true, ahora + 300, ahora) == ORDEN_OK);
  VERIFICAR(strcmp(orden_motivo(ORDEN_VENCIDA), "orden_vencida") == 0);

  // aviso_s: 0 o de 5 a 600
  VERIFICAR(aviso_valido(0) && aviso_valido(5) && aviso_valido(600));
  VERIFICAR(!aviso_valido(4) && !aviso_valido(601) && !aviso_valido(-1));

  RESUMEN();
}
