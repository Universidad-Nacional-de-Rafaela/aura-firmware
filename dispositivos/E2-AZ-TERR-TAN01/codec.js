// Codec de ChirpStack v4 para E2-AZ-TERR-TAN01: nivel de tres tanques de agua (clase A).
// Es el par de E2-AZ-TERR-TAN01.ino: si cambia el formato de los bytes de uno, cambia el otro.
// Sigue el mismo esquema que el codec de E1-AZ-TERR-TAN01 (puertos, ref y reporte de comandos).
// Lo que devuelve decodeUplink es el "object" del uplink: el lorawan-bridge lo publica como
// "values" en hw/eui-<DevEUI>/data, o como "response" si es un reporte de comando
// (contrato v4.0, §3.1, §3.3 y §3.4).
//
// SUBIDA
// fPort 1, 6 bytes, medicion (big endian):
//   0-1 distancia del sensor al agua, tanque 1, en cm x 10 (= mm) (uint16)
//   2-3 lo mismo para el tanque 2
//   4-5 lo mismo para el tanque 3
//   values la trae en cm con un decimal: distancia_tanqueN_cm (2462 -> 246.2).
//   0xFFFE = el agua esta dentro de la zona ciega del sensor (tanque lleno). No es una falla:
//   se informa 25 cm, el nivel maximo que el sensor puede medir (saturacion del rango, como
//   un transmisor 4-20 mA en su tope). Quiere decir "25 cm o menos".
//   0xFFFF sin eco, 0xFFFD demasiado lejos, o cualquier otro valor fuera de 25..600 cm: es una
//   falla y el campo del tanque no aparece en values (§3.1).
//   Los nombres de los campos son los de conector.toml.
// fPort 2, 4 bytes, reporte de un comando:
//   0   ref del comando (el que llego en el downlink)
//   1   resultado: 0 aplicado, 1 fuera de rango, 2 comando desconocido
//   2-3 intervalo de envio vigente, en segundos (uint16)
//
// BAJADA (fPort 10). El lorawan-bridge pasa {command, params, ref}:
//   medir_ahora                            -> 01 ref
//   set_config {"intervalo_s": 60..43200}  -> 02 ref intervalo_hi intervalo_lo
// El nodo contesta cada comando con un reporte en el fPort 2. Despues de medir_ahora mide los
// tres tanques y manda ademas la medicion en el fPort 1. intervalo_s es cada cuanto mide los
// tres tanques y envia: el nodo reparte los turnos cada intervalo_s / 3.

var N_TANQUES = 3;
var DIST_MIN_MM = 250;            // en los bytes (mm = cm x 10); iguales al sketch
var DIST_MAX_MM = 6000;           // en values: 25 a 600 cm, como conector.toml
var INTERVALO_MIN_S = 60;
var INTERVALO_MAX_S = 43200;      // 12 h; el uplink interval del device profile tiene que ser este maximo
var MOTIVOS = { 1: "fuera_de_rango", 2: "comando_desconocido" };
var ZONA_CIEGA = 0xFFFE;          // el firmware lo manda cuando el agua esta mas cerca que 250 mm
var FALLAS = { 0xFFFF: "sin eco", 0xFFFD: "demasiado lejos" };

function u16(bytes, i) {
  return (bytes[i] << 8) | bytes[i + 1];
}

function cm(mm) {
  return mm / 10;                 // 2462 -> 246.2: un decimal exacto, sin redondeo
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
      values["distancia_tanque" + n + "_cm"] = cm(dist);
    } else if (dist === ZONA_CIEGA) {
      values["distancia_tanque" + n + "_cm"] = cm(DIST_MIN_MM);
      warnings.push("tanque " + n + ": agua en la zona ciega (lleno), se informa " + cm(DIST_MIN_MM) + " cm");
    } else {
      warnings.push("tanque " + n + ": " + (FALLAS[dist] || "fuera de rango (" + cm(dist) + " cm)"));
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
    return { bytes: [0x02, ref, (s >> 8) & 0xff, s & 0xff] };
  }
  return { errors: ["comando desconocido: " + d.command] };
}

// ChirpStack no define "module": solo existe cuando se prueba el codec con Node (tests/).
if (typeof module !== "undefined") {
  module.exports = { decodeUplink: decodeUplink, encodeDownlink: encodeDownlink };
}
