// Codec de ChirpStack v4 para E2-AZ-TERR-TAN01: nivel de tres tanques de agua (clase A).
// Es el par de E2-AZ-TERR-TAN01.ino: si cambia el formato de los bytes de uno, cambia el otro.
// Sigue el mismo esquema que el codec de E1-AZ-TERR-TAN01 (puertos, ref y reporte de comandos).
// Lo que devuelve decodeUplink es el "object" del uplink: el lorawan-bridge lo publica como
// "values" en hw/eui-<DevEUI>/data, o como "response" si es un reporte de comando
// (contrato v4.0, §3.1, §3.3 y §3.4).
//
// SUBIDA
// fPort 1, 6 bytes, medicion (big endian):
//   0-1 distancia del sensor al agua, tanque 1, en mm (uint16)
//   2-3 lo mismo para el tanque 2
//   4-5 lo mismo para el tanque 3
//   Sin medicion valida: 0xFFFF sin eco, 0xFFFE demasiado cerca, 0xFFFD demasiado lejos.
//   Esos valores, y cualquiera fuera de 250..6000 mm, no van en values: el campo del tanque no
//   aparece (§3.1). Los nombres de los campos son los de conector.toml.
// fPort 2, 4 bytes, reporte de un comando:
//   0   ref del comando (el que llego en el downlink)
//   1   resultado: 0 aplicado, 1 fuera de rango, 2 comando desconocido
//   2-3 intervalo de envio vigente, en segundos (uint16)
//
// BAJADA (fPort 10). El lorawan-bridge pasa {command, params, ref}:
//   medir_ahora                            -> 01 ref
//   set_config {"intervalo_s": 60..43200}  -> 02 ref intervalo_hi intervalo_lo
// El nodo contesta cada comando con un reporte en el fPort 2. Despues de medir_ahora mide los
// tres tanques y manda ademas la medicion en el fPort 1. El nodo redondea intervalo_s hacia
// arriba a un multiplo de 60; el reporte trae el valor que quedo vigente.

var N_TANQUES = 3;
var DIST_MIN_MM = 250;            // iguales a conector.toml y al sketch
var DIST_MAX_MM = 6000;
var INTERVALO_MIN_S = 60;
var INTERVALO_MAX_S = 43200;      // 12 h; el uplink interval del device profile tiene que ser este maximo
var CICLO_S = 60;
var MOTIVOS = { 1: "fuera_de_rango", 2: "comando_desconocido" };
var FALLAS = { 0xFFFF: "sin eco", 0xFFFE: "demasiado cerca", 0xFFFD: "demasiado lejos" };

function u16(bytes, i) {
  return (bytes[i] << 8) | bytes[i + 1];
}

function decodeUplink(input) {
  var b = input.bytes;
  if (input.fPort === 2) {
    if (b.length !== 4) return { errors: ["reporte: " + b.length + " bytes, se esperaban 4"] };
    var rep = { ref: b[0], aplicado: b[1] === 0, config: { intervalo_s: u16(b, 2) } };
    if (b[1] !== 0) rep.motivo = MOTIVOS[b[1]] || "rechazado_por_el_nodo";
    return { data: rep };
  }
  if (input.fPort !== 1) {
    return { errors: ["fPort " + input.fPort + " desconocido (1 medicion, 2 reporte)"] };
  }
  if (b.length !== 2 * N_TANQUES) {
    return { errors: ["medicion: " + b.length + " bytes, se esperaban " + 2 * N_TANQUES] };
  }

  var values = {};
  var warnings = [];
  for (var n = 1; n <= N_TANQUES; n++) {
    var dist = u16(b, 2 * (n - 1));
    if (dist >= DIST_MIN_MM && dist <= DIST_MAX_MM) {
      values["distancia_tanque" + n + "_mm"] = dist;
    } else {
      warnings.push("tanque " + n + ": " + (FALLAS[dist] || "fuera de rango (" + dist + " mm)"));
    }
  }
  // Si ningun tanque dio una medicion valida, no se publica data (§3.1).
  if (Object.keys(values).length === 0) {
    return { errors: ["sin lecturas validas en ningun tanque: " + warnings.join(", ")] };
  }
  return { data: values, warnings: warnings };
}

function encodeDownlink(input) {
  var d = input.data || {};
  var ref = d.ref;
  if (typeof ref !== "number" || ref % 1 !== 0 || ref < 1 || ref > 255) {
    return { errors: ["falta ref (1-255): lo pone el lorawan-bridge"] };
  }
  if (d.command === "medir_ahora") {
    return { bytes: [0x01, ref] };
  }
  if (d.command === "set_config") {
    var p = d.params || {};
    var claves = Object.keys(p);
    for (var i = 0; i < claves.length; i++) {
      if (claves[i] !== "intervalo_s") {
        return { errors: ["set_config: parametro desconocido '" + claves[i] + "' (solo intervalo_s)"] };
      }
    }
    var s = p.intervalo_s;
    if (typeof s !== "number" || s % 1 !== 0 || s < INTERVALO_MIN_S || s > INTERVALO_MAX_S) {
      return { errors: ["set_config: intervalo_s tiene que ser un entero entre " + INTERVALO_MIN_S + " y " + INTERVALO_MAX_S] };
    }
    var res = { bytes: [0x02, ref, (s >> 8) & 0xff, s & 0xff] };
    if (s % CICLO_S !== 0) {
      res.warnings = ["el nodo redondea intervalo_s a " + Math.ceil(s / CICLO_S) * CICLO_S + " s (multiplo de " + CICLO_S + ")"];
    }
    return res;
  }
  return { errors: ["comando desconocido: " + d.command] };
}

// ChirpStack no define "module": solo existe cuando se prueba el codec con Node (tests/).
if (typeof module !== "undefined") {
  module.exports = { decodeUplink: decodeUplink, encodeDownlink: encodeDownlink };
}
