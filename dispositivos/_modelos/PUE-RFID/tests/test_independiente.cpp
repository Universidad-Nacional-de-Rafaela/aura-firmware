#include "../main/independiente_logica.h"
#include "../llaveros/llaveros.ejemplo.h"
#include "../../../../comun/tests/aserciones.h"
#include <cstdlib>
#include <vector>

static const uint8_t A[4] = {0xAB, 0xE8, 0xB0, 0x22};
static const uint8_t B[4] = {0x4B, 0x3E, 0xC2, 0x22};

#define CANTIDAD_EJEMPLO (sizeof(LLAVEROS) / sizeof(LLAVEROS[0]))
static_assert(CANTIDAD_EJEMPLO >= 1 && CANTIDAD_EJEMPLO <= LISTA_MAX, "el ejemplo tiene que compilar en la placa");

// Saca lo que recibiria main_independiente.c
static std::vector<accion_t> vaciar(puerta_t &p) {
  std::vector<accion_t> v;
  accion_t a;
  while (independiente_sacar(&p, &a)) v.push_back(a);
  return v;
}

static int contar(const std::vector<accion_t> &v, accion_tipo_t tipo) {
  int n = 0;
  for (auto &a : v) n += a.tipo == tipo;
  return n;
}

static bool hay_sonido(const std::vector<accion_t> &v, sonido_t s) {
  for (auto &a : v)
    if (a.tipo == ACC_SONIDO && a.valor == s) return true;
  return false;
}

static bool hay_aviso(const std::vector<accion_t> &v, aviso_t m) {
  for (auto &a : v)
    if (a.tipo == ACC_AVISO && a.valor == m) return true;
  return false;
}

static bool todas_locales(const std::vector<accion_t> &v) {
  for (auto &a : v)
    if (!independiente_accion_local(a.tipo)) return false;
  return true;
}

static puerta_t nueva(bool abierta = false) {
  puerta_t p;
  independiente_iniciar(&p, 0, abierta);
  return p;
}

int main() {
  // ---- La lista compilada: el ejemplo carga entero y abre solo con sus UID ----
  {
    lista_t l;
    size_t primera = 99;
    VERIFICAR(independiente_cargar_lista(&l, LLAVEROS, CANTIDAD_EJEMPLO, &primera) == 0);
    VERIFICAR(primera == 0);
    VERIFICAR(l.n == (int)CANTIDAD_EJEMPLO);
    for (size_t i = 0; i < CANTIDAD_EJEMPLO; i++) {
      uint8_t uid[UID_MAX_BYTES];
      uint8_t largo = 0;
      VERIFICAR(hex_a_uid(LLAVEROS[i], uid, &largo));
      VERIFICAR(lista_autorizada(&l, uid, largo));
    }
    VERIFICAR(!lista_autorizada(&l, A, 4));
  }

  // ---- Entradas que no sirven: se cuentan, se dice cual es la primera y las demas se cargan ----
  {
    static const char *const mezcla[] = {"ABE8B022", "ABE8 B022", "abe8b022", "123456", "4B3EC222", ""};
    lista_t l;
    size_t primera;
    VERIFICAR(independiente_cargar_lista(&l, mezcla, 6, &primera) == 4);   // espacio, repetida, corta y vacia
    VERIFICAR(primera == 2);
    VERIFICAR(l.n == 2);
    VERIFICAR(lista_autorizada(&l, A, 4));
    VERIFICAR(lista_autorizada(&l, B, 4));
  }

  // ---- No entran mas de LISTA_MAX, y la carga no deja nada de una lista anterior ----
  {
    char textos[LISTA_MAX + 1][16];
    const char *uids[LISTA_MAX + 1];
    for (int i = 0; i <= LISTA_MAX; i++) {
      snprintf(textos[i], sizeof(textos[i]), "000000%02X", i + 1);
      uids[i] = textos[i];
    }
    lista_t l;
    lista_vaciar(&l);
    lista_agregar(&l, A, 4);
    size_t primera;
    VERIFICAR(independiente_cargar_lista(&l, uids, LISTA_MAX + 1, &primera) == 1);
    VERIFICAR(primera == LISTA_MAX + 1);
    VERIFICAR(l.n == LISTA_MAX);
    VERIFICAR(!lista_autorizada(&l, A, 4));
  }

  // ---- Un llavero de la lista: pulso de siempre, pitido de OK y se vuelve a trabar solo ----
  {
    puerta_t p = nueva();
    independiente_tarjeta(&p, 1000, A, 4, true);
    auto v = vaciar(p);
    VERIFICAR(todas_locales(v));
    VERIFICAR(contar(v, ACC_PULSO) == 1);
    for (auto &a : v)
      if (a.tipo == ACC_PULSO) VERIFICAR(a.valor == PULSO_DEFECTO_MS);
    VERIFICAR(hay_sonido(v, SON_OK));
    VERIFICAR(hay_aviso(v, AVISO_PITIDOS_PULSO));
    puerta_tick(&p, 1000 + PULSO_DEFECTO_MS - 1);
    VERIFICAR(contar(vaciar(p), ACC_CORTAR_PULSO) == 0);
    puerta_tick(&p, 1000 + PULSO_DEFECTO_MS);
    VERIFICAR(contar(vaciar(p), ACC_CORTAR_PULSO) == 1);
    VERIFICAR(!p.cerradura.energizada);
    VERIFICAR(p.cola_cant == 0);
  }

  // ---- Sin armado: dos pasadas seguidas son dos accesos ----
  {
    puerta_t p = nueva();
    independiente_tarjeta(&p, 1000, A, 4, true);
    vaciar(p);
    independiente_tarjeta(&p, 2000, A, 4, true);        // en la conectada, esto armaria
    auto v = vaciar(p);
    VERIFICAR(!p.armada);
    VERIFICAR(!p.doble_activa);
    VERIFICAR(contar(v, ACC_PULSO) == 1);
    VERIFICAR(hay_sonido(v, SON_OK));
    VERIFICAR(!hay_sonido(v, SON_ARMADO));

    // ---- Sin intrusion: la puerta se abre y no hay sirena; se corta el pulso ----
    puerta_sensor(&p, 2500, true);
    v = vaciar(p);
    VERIFICAR(todas_locales(v));
    VERIFICAR(contar(v, ACC_CORTAR_PULSO) == 1);
    VERIFICAR(!hay_aviso(v, AVISO_INTRUSION));
    VERIFICAR(!p.intrusion);
  }

  // ---- Un llavero desconocido: pitidos graves y la cerradura no se toca ----
  {
    puerta_t p = nueva();
    independiente_tarjeta(&p, 1000, B, 4, false);
    auto v = vaciar(p);
    VERIFICAR(todas_locales(v));
    VERIFICAR(hay_sonido(v, SON_RECHAZO));
    VERIFICAR(contar(v, ACC_PULSO) == 0);
    VERIFICAR(!p.cerradura.energizada);
  }

  // ---- Muchos rechazos: el lector se frena igual que en la conectada ----
  {
    puerta_t p = nueva();
    int64_t t = 1000;
    for (int i = 0; i < RECHAZOS_PARA_BLOQUEO; i++, t += 1000) independiente_tarjeta(&p, t, B, 4, false);
    VERIFICAR(todas_locales(vaciar(p)));
    VERIFICAR(p.bloqueado);
    independiente_tarjeta(&p, t, A, 4, true);
    VERIFICAR(vaciar(p).empty());
    independiente_tarjeta(&p, t + BLOQUEO_MS, A, 4, true);
    VERIFICAR(contar(vaciar(p), ACC_PULSO) == 1);
  }

  // ---- El limite por minuto de la cerradura sigue valiendo ----
  {
    puerta_t p = nueva();
    int pulsos = 0, errores = 0;
    for (int64_t t = 0; t < VENTANA_LIMITE_MS; t += 10) {
      if (t % 4000 == 0) independiente_tarjeta(&p, t, A, 4, true);
      puerta_tick(&p, t);
      auto v = vaciar(p);
      pulsos += contar(v, ACC_PULSO);
      errores += hay_sonido(v, SON_ERROR);
      VERIFICAR(cerradura_usado_ms(&p.cerradura, t) <= LIMITE_MINUTO_MS);
    }
    VERIFICAR(pulsos == 7);                              // 6 de 3 s y uno recortado a 2 s
    VERIFICAR(errores > 0);
  }

  // ---- Puerta abierta de mas: aviso a los INDEPENDIENTE_AVISO_S, con tope, y se calla al cerrar ----
  {
    puerta_t p = nueva();
    puerta_sensor(&p, 1000, true);
    vaciar(p);
    int64_t suena = 1000 + (int64_t)INDEPENDIENTE_AVISO_S * 1000;
    puerta_tick(&p, suena - 1);
    VERIFICAR(vaciar(p).empty());
    puerta_tick(&p, suena);
    auto v = vaciar(p);
    VERIFICAR(todas_locales(v));
    VERIFICAR(hay_aviso(v, AVISO_PUERTA_ABIERTA));
    puerta_tick(&p, suena + TOPE_AVISO_MS);
    VERIFICAR(hay_aviso(vaciar(p), AVISO_NADA));         // tope: se calla aunque siga abierta

    puerta_sensor(&p, suena + TOPE_AVISO_MS + 1000, false);
    vaciar(p);
    puerta_sensor(&p, 200000, true);                     // abrirla de nuevo vuelve a contar
    puerta_tick(&p, 200000 + (int64_t)INDEPENDIENTE_AVISO_S * 1000);
    VERIFICAR(hay_aviso(vaciar(p), AVISO_PUERTA_ABIERTA));
    puerta_sensor(&p, 230000, false);
    v = vaciar(p);
    VERIFICAR(todas_locales(v));
    VERIFICAR(hay_aviso(v, AVISO_NADA));
  }

  // ---- Cerrada a tiempo: no suena nada ----
  {
    puerta_t p = nueva();
    puerta_sensor(&p, 1000, true);
    puerta_sensor(&p, 5000, false);
    puerta_tick(&p, 1000 + (int64_t)INDEPENDIENTE_AVISO_S * 1000);
    VERIFICAR(!hay_aviso(vaciar(p), AVISO_PUERTA_ABIERTA));
  }

  // ---- Arranca con la puerta abierta: el aviso local, sin intrusion ----
  {
    puerta_t p = nueva(true);
    VERIFICAR(!p.armada);
    VERIFICAR(vaciar(p).empty());
    puerta_tick(&p, (int64_t)INDEPENDIENTE_AVISO_S * 1000);
    auto v = vaciar(p);
    VERIFICAR(hay_aviso(v, AVISO_PUERTA_ABIERTA));
    VERIFICAR(!hay_aviso(v, AVISO_INTRUSION));
  }

  // ---- Con la puerta abierta un llavero no da pulso, y calla el aviso ----
  {
    puerta_t p = nueva(true);
    puerta_tick(&p, (int64_t)INDEPENDIENTE_AVISO_S * 1000);
    vaciar(p);
    independiente_tarjeta(&p, 20000, A, 4, true);
    auto v = vaciar(p);
    VERIFICAR(contar(v, ACC_PULSO) == 0);
    VERIFICAR(hay_aviso(v, AVISO_NADA));
    VERIFICAR(hay_sonido(v, SON_OK));
  }

  // ---- Hechos al azar durante horas: nunca se arma, nada sale de la placa y el limite se cumple ----
  {
    srand(1);
    puerta_t p = nueva();
    bool abierta = false, ok = true;
    int pulsos = 0, avisos = 0;
    // De a 10 ms, el periodo de la tarea de la logica: 5 horas y media
    for (int64_t t = 0; t < 20000000; t += 10) {
      int que = rand() % 3000;
      // Con la puerta abierta casi no se pasan llaveros: cada uno reinicia la cuenta del aviso
      if (que < 15 && (!abierta || rand() % 20 == 0)) independiente_tarjeta(&p, t, A, 4, true);
      else if (que < 18) independiente_tarjeta(&p, t, B, 4, false);
      else if (que == 18) puerta_sensor(&p, t, abierta = !abierta);
      puerta_tick(&p, t);
      auto v = vaciar(p);
      ok = ok && todas_locales(v) && !hay_aviso(v, AVISO_INTRUSION) && !hay_sonido(v, SON_ARMADO) &&
           !p.armada && !p.intrusion && p.perdidas == 0 &&
           cerradura_usado_ms(&p.cerradura, t) <= LIMITE_MINUTO_MS;
      for (auto &a : v) {
        if (a.tipo == ACC_PULSO) {
          pulsos++;
          ok = ok && a.valor <= PULSO_DEFECTO_MS;
        }
        if (a.tipo == ACC_AVISO && a.valor == AVISO_PUERTA_ABIERTA) avisos++;
      }
    }
    VERIFICAR(ok);
    VERIFICAR(pulsos > 100);              // la prueba paso por lo que dice probar
    VERIFICAR(avisos > 10);
  }

  RESUMEN();
}
