// Codec del device profile `aura-clase-a` para E2-AZ-TERR-TAN01 (3 tanques de agua).
// Se pega en ChirpStack (Device profile > Codec > JavaScript functions) y se versiona acá
// para que se revise junto con el conector. Si cambia uno, cambia el otro.
//
// Uplink, puerto 1, 10 bytes, big-endian:
//   [0..1] contador de lectura   [2..3] tanque 1   [4..5] tanque 2   [6..7] tanque 3
//   [8..9] reservado
// Cada tanque es la distancia del sensor al agua, en mm. Si el sensor falla, el firmware manda
// un valor especial: 0xFFFF (sin eco), 0xFFFE (demasiado cerca), 0xFFFD (demasiado lejos).
//
// Contrato §3.1: `data` es lo que el lorawan-bridge publica como `values`. Solo mediciones:
// un sensor que falla NO aparece, y no van el contador, el reservado ni estados. Lo que no
// es una medición va en `warnings`, que ChirpStack muestra en el evento pero no llega a AURA.

var DIST_MIN_MM = 250;    // igual que DIST_MIN_MM de E2-AZ-TERR-TAN01.ino y conector.toml
var DIST_MAX_MM = 6000;   // igual que DIST_MAX_MM de E2-AZ-TERR-TAN01.ino y conector.toml
var N_TANQUES = 3;
var TAM_UPLINK = 10;
var FALLAS = { 0xFFFF: "sin eco", 0xFFFE: "demasiado cerca", 0xFFFD: "demasiado lejos" };

function decodeUplink(input) {
  var b = input.bytes;
  if (input.fPort !== 1) {
    // TODO: el reporte bajo demanda llega por el puerto 2 y es una respuesta a un comando,
    // no una medición nueva. Hasta que se defina cómo se decodifica, no se publica nada.
    return { errors: ["fPort " + input.fPort + " sin soporte: solo el puerto 1 es telemetría"] };
  }
  if (b.length !== TAM_UPLINK) {
    return { errors: ["largo inesperado: " + b.length + " bytes, se esperaban " + TAM_UPLINK] };
  }

  var data = {};
  var warnings = [];
  for (var i = 0; i < N_TANQUES; i++) {
    var mm = (b[2 + 2 * i] << 8) | b[3 + 2 * i];
    if (mm >= DIST_MIN_MM && mm <= DIST_MAX_MM) {
      data["distancia_tanque" + (i + 1) + "_mm"] = mm;
    } else {
      var motivo = FALLAS[mm] || ("valor fuera de rango: " + mm);
      warnings.push("tanque " + (i + 1) + " sin medición válida (" + motivo + ")");
    }
  }
  return { data: data, warnings: warnings };
}

// El lorawan-bridge pasa el comando de AURA como `input.data` (contrato §3.3):
//   { "command": "set_config", "params": { "intervalo_s": 300 } }
// El nodo recibe 2 bytes big-endian con los segundos (60 a 65535; él lo redondea a múltiplos
// de 60). Cualquier otro comando se rechaza: el nodo no lo entendería.
function encodeDownlink(input) {
  var d = input.data || {};
  if (d.command !== "set_config") {
    return { errors: ["comando no soportado: " + d.command] };
  }
  var p = d.params || {};
  var claves = Object.keys(p);
  if (claves.length !== 1 || claves[0] !== "intervalo_s") {
    return { errors: ["set_config solo acepta intervalo_s"] };
  }
  var s = p.intervalo_s;
  if (typeof s !== "number" || Math.floor(s) !== s || s < 60 || s > 65535) {
    return { errors: ["intervalo_s tiene que ser un entero de 60 a 65535"] };
  }
  return { bytes: [(s >> 8) & 0xFF, s & 0xFF], fPort: 1 };
}
