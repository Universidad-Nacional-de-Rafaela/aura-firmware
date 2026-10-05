#pragma once
#include <cstdio>
#include <cstring>

static int g_fallos = 0;
static int g_corridos = 0;

#define VERIFICAR(cond)                                                    \
  do {                                                                     \
    g_corridos++;                                                          \
    if (!(cond)) {                                                         \
      g_fallos++;                                                          \
      printf("  FALLO %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                      \
  } while (0)

#define RESUMEN()                                                          \
  do {                                                                     \
    printf("%d verificaciones, %d fallos\n", g_corridos, g_fallos);        \
    return g_fallos == 0 ? 0 : 1;                                          \
  } while (0)
