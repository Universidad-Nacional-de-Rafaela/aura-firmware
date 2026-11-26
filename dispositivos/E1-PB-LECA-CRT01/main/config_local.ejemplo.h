/*
 * Plantilla de config_local.h. Copiala como config_local.h (en esta carpeta) y completa
 * el ID y la clave de la mesh, que los da la catedra. NO se sube al repositorio.
 */
#pragma once

#define MESH_ID_BYTES   0x00, 0x00, 0x00, 0x00, 0x00, 0x00     /* 6 bytes */
#define MESH_AP_PASS    "CAMBIAR"                               /* minimo 8 caracteres */

/* 1 = firmware de banco (consola serie, sin malla). 0 o sin definir = firmware real. */
#define AURA_MODO_BANCO 0
