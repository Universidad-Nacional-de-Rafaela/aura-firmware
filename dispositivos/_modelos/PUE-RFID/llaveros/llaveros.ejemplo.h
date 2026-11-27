/*
 * Lista de llaveros de EJEMPLO para el modo independiente: los UID son inventados y no abren
 * ninguna puerta real.
 *
 * Para cada puerta, copiar este archivo en esta carpeta como <codigo>.h (por ejemplo,
 * E2-P2-RECT-PUE01.h), poner sus UID y compilar con CONFIG_PUERTA_CODIGO igual al nombre del
 * archivo sin ".h". Esos archivos estan ignorados por git: el repo es publico y un UID asociado a
 * una persona es un dato personal. Nunca se suben.
 *
 * Entre 1 y 32 UID, como los imprime lector_banco/: hexadecimal, sin separadores, de 4, 7 o 10
 * bytes (8, 14 o 20 digitos). Uno por linea, entre comillas y con su coma.
 */
#pragma once

static const char *const LLAVEROS[] = {
    "00000001",
    "04000000000002",
    "00000000000000000003",
};
