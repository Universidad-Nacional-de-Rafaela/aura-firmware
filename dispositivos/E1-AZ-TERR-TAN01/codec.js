// Codec de ChirpStack v4 para el sensor de nivel de dos tanques de agua (clase C).
// Lo que devuelve decodeUplink es el "object" del uplink: el lorawan-bridge lo publica como
// "values" en hw/eui-<DevEUI>/data, o como "response" si es un reporte de comando
// (contrato v4.0, §3.1, §3.3 y §3.4).
//
// SUBIDA
//   fPort 1, 4 bytes, medicion (big endian):
//     0-1  distancia del sensor al agua, tanque 1, en cm x 10 (uint16); 0xFFFF = sin lectura
//     2-3  lo mismo para el tanque 2
//   fPort 2, 4 bytes, reporte de un comando:
//     0    ref del comando (el que llego en el downlink)
//     1    resultado: 0 aplicado, 1 fuera de rango, 2 comando desconocido
//     2-3  intervalo de lectura vigente, en segundos (uint16)
//
// BAJADA (fPort 10). El lorawan-bridge pasa {command, params, ref}:
//     medir_ahora                          ->  01 ref
//     set_config {"intervalo_s": 60..43200}  ->  02 ref intervalo_hi intervalo_lo
// El nodo contesta cada comando con un reporte en el fPort 2. Despues de medir_ahora
// manda ademas la medicion en el fPort 1.

var SIN_LECTURA = 0xFFFF;
var INTERVALO_MIN_S = 60;
var INTERVALO_MAX_S = 43200;   // 12 h; el uplink interval del device profile tiene que ser este maximo
var MOTIVOS = { 1: "fuera_de_rango", 2: "comando_desconocido" };

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
  if (b.length !== 4) {
    return { errors: ["medicion: " + b.length + " bytes, se esperaban 4"] };
  }
  var values = {};
  for (var n = 1; n <= 2; n++) {
    var dist = u16(b, 2 * (n - 1));
    if (dist !== SIN_LECTURA) values["dist_t" + n + "_cm"] = dist / 10.0;
  }
  if (Object.keys(values).length === 0) {
    return { errors: ["sin lecturas validas en ningun tanque"] };
  }
  return { data: values };
}

function encodeDownlink(input) {
  var d = input.data || {};
  var ref = d.ref;
  if (typeof ref !== "number" || ref < 1 || ref > 255) {
    return { errors: ["falta ref (1-255): lo pone el lorawan-bridge"] };
  }
  if (d.command === "medir_ahora") {
    return { bytes: [0x01, ref] };
  }
  if (d.command === "set_config") {
    var s = d.params && d.params.intervalo_s;
    if (typeof s !== "number" || s % 1 !== 0 || s < INTERVALO_MIN_S || s > INTERVALO_MAX_S) {
      return { errors: ["set_config: intervalo_s tiene que ser un entero entre " + INTERVALO_MIN_S + " y " + INTERVALO_MAX_S] };
    }
    return { bytes: [0x02, ref, (s >> 8) & 0xff, s & 0xff] };
  }
  return { errors: ["comando desconocido: " + d.command] };
}
