/*
 * Codec de ChirpStack v4 - E2-AZ-TERR-TAN01 (nivel de 3 tanques de agua)
 * Device profile -> Codec -> "JavaScript functions". Reemplaza al codec anterior.
 * Es el par de nodo_tanques.ino: si cambia el formato de bytes de uno, cambia el otro.
 *
 * ---------------- SUBIDA (nodo -> AURA) ----------------
 * Puerto 1, telemetria, 10 bytes:
 *   [0..1] contador   [2..3] tanque 1   [4..5] tanque 2   [6..7] tanque 3   [8..9] reservado
 *   Distancia en mm, big-endian. 0xFFFF sin eco, 0xFFFE demasiado cerca, 0xFFFD demasiado lejos.
 *   data = solo las distancias validas (250 a 6000 mm), con los nombres de conector.toml.
 *   Un sensor que falla NO aparece en data (contrato MQTT v4.0, seccion 3.1): se avisa en
 *   "warnings". El contador y el campo reservado tampoco van en data.
 *
 * Puerto 2, respuesta a un pedido de reporte (NO es telemetria):
 *   [0] 0x01   [1] mascara de tanques   y por cada tanque de la mascara: distancia (2) + edad_s (2)
 *   data = { respuesta: "reporte", tanques: { tanque1: { distancia_mm, estado, edad_s }, ... } }
 *   Los nombres son distintos a los de la telemetria a proposito: un reporte son datos que el
 *   nodo ya tenia en RAM y no tienen que guardarse como una medicion nueva.
 *
 * ---------------- BAJADA (AURA -> nodo) ----------------
 * encodeDownlink recibe el JSON del contrato (seccion 3.3) en input.data:
 *   { "command": "set_config",  "params": { "intervalo_s": 300 } }   -> 2 bytes (segundos, big-endian)
 *   { "command": "medir_hora",  "params": { "tanques": [1, 3] } }    -> 1 byte (mascara de tanques)
 *   { "command": "medir_hora" }                                      -> todos los tanques
 * medir_hora pide un reporte de lo que el nodo tiene en RAM. No mide ni altera el ciclo de 20 s.
 * El firmware distingue los comandos por la longitud: 2 bytes = set_config, 1 byte = reporte.
 *
 * Clase A: el nodo recibe el comando en la ventana que sigue a su proximo uplink, o sea hasta
 * intervalo_s despues de encolarlo (60 s por defecto).
 */

var N_TANQUES = 3;
var MASCARA_TODOS = (1 << N_TANQUES) - 1;

var PUERTO_TELEMETRIA = 1;
var PUERTO_REPORTE = 2;
var PUERTO_COMANDOS = 1;          // cualquier puerto distinto de 0; el firmware no lo mira
var TIPO_REPORTE_MEDICIONES = 0x01;

var TAM_TELEMETRIA = 10;
var DIST_MIN_MM = 250;            // iguales a conector.toml y a nodo_tanques.ino
var DIST_MAX_MM = 6000;
var SIN_ECO = 0xFFFF;
var DEMASIADO_CERCA = 0xFFFE;
var DEMASIADO_LEJOS = 0xFFFD;
var SIN_MEDIR = 0xFFFF;           // edad: ese tanque todavia no se midio desde el arranque

var INTERVALO_MIN_S = 60;         // una medicion por tanque por minuto
var INTERVALO_MAX_S = 65535;
var CICLO_S = 60;

// Nombres con los que AURA puede pedir el reporte. Para cambiar el nombre, editar esta lista.
var COMANDOS_REPORTE = ["medir_hora"];

function u16(b, i) {
  return (b[i] << 8) | b[i + 1];
}

function estadoDe(valor) {
  if (valor >= DIST_MIN_MM && valor <= DIST_MAX_MM) return "ok";
  if (valor === SIN_ECO) return "sin_eco";
  if (valor === DEMASIADO_CERCA) return "demasiado_cerca";
  if (valor === DEMASIADO_LEJOS) return "demasiado_lejos";
  return "fuera_de_rango";
}

// ======================= SUBIDA =======================
function decodeUplink(input) {
  var bytes = input.bytes;
  if (input.fPort === PUERTO_TELEMETRIA) return decodificarTelemetria(bytes);
  if (input.fPort === PUERTO_REPORTE) return decodificarReporte(bytes);
  return { errors: ["puerto " + input.fPort + " desconocido (se esperaba 1 o 2)"] };
}

function decodificarTelemetria(bytes) {
  if (bytes.length !== TAM_TELEMETRIA) {
    return { errors: ["telemetria: se esperaban " + TAM_TELEMETRIA + " bytes y llegaron " + bytes.length] };
  }
  var data = {};
  var warnings = [];
  for (var i = 0; i < N_TANQUES; i++) {
    var valor = u16(bytes, 2 + 2 * i);
    var estado = estadoDe(valor);
    if (estado === "ok") {
      data["distancia_tanque" + (i + 1) + "_mm"] = valor;
    } else {
      warnings.push("tanque " + (i + 1) + ": " + estado + " (sin medicion valida)");
    }
  }
  return { data: data, warnings: warnings };
}

function decodificarReporte(bytes) {
  if (bytes.length < 2 || bytes[0] !== TIPO_REPORTE_MEDICIONES) {
    return { errors: ["reporte: tipo desconocido o mensaje vacio"] };
  }
  var mascara = bytes[1];
  var cantidad = 0;
  for (var i = 0; i < N_TANQUES; i++) {
    if (mascara & (1 << i)) cantidad++;
  }
  if (cantidad === 0 || (mascara & ~MASCARA_TODOS) !== 0) {
    return { errors: ["reporte: mascara invalida 0x" + mascara.toString(16)] };
  }
  if (bytes.length !== 2 + 4 * cantidad) {
    return { errors: ["reporte: se esperaban " + (2 + 4 * cantidad) + " bytes y llegaron " + bytes.length] };
  }

  var tanques = {};
  var n = 2;
  for (var t = 0; t < N_TANQUES; t++) {
    if (!(mascara & (1 << t))) continue;
    var dist = u16(bytes, n);
    var edad = u16(bytes, n + 2);
    n += 4;

    var nunca = (edad === SIN_MEDIR);
    var estado = nunca ? "sin_medir" : estadoDe(dist);
    tanques["tanque" + (t + 1)] = {
      distancia_mm: estado === "ok" ? dist : null,
      estado: estado,
      edad_s: nunca ? null : edad   // 65534 significa "65534 s o mas"
    };
  }
  return { data: { respuesta: "reporte", tanques: tanques } };
}

// ======================= BAJADA =======================
function encodeDownlink(input) {
  var d = input.data || {};
  var params = d.params || {};
  if (d.command === "set_config") return codificarSetConfig(params);
  if (COMANDOS_REPORTE.indexOf(d.command) >= 0) return codificarReporte(params);
  return { errors: ["comando desconocido: " + d.command] };
}

// Si algo no valida, no se encola nada (contrato 3.3: no se aplica nada del comando).
function codificarSetConfig(params) {
  var claves = Object.keys(params);
  if (claves.length === 0) {
    return { errors: ["set_config sin parametros"] };
  }
  for (var i = 0; i < claves.length; i++) {
    if (claves[i] !== "intervalo_s") {
      return { errors: ["set_config: parametro desconocido '" + claves[i] + "' (solo intervalo_s)"] };
    }
  }

  var s = params.intervalo_s;
  if (typeof s !== "number" || Math.floor(s) !== s) {
    return { errors: ["intervalo_s tiene que ser un numero entero de segundos"] };
  }
  if (s < INTERVALO_MIN_S || s > INTERVALO_MAX_S) {
    return { errors: ["intervalo_s=" + s + " fuera de rango (" + INTERVALO_MIN_S + " a " + INTERVALO_MAX_S + ")"] };
  }

  var warnings = [];
  if (s % CICLO_S !== 0) {
    warnings.push("el nodo redondea intervalo_s hacia arriba a un multiplo de " + CICLO_S +
                  " s: va a usar " + (Math.ceil(s / CICLO_S) * CICLO_S) + " s");
  }
  return { bytes: [(s >> 8) & 0xFF, s & 0xFF], fPort: PUERTO_COMANDOS, warnings: warnings };
}

// params.tanques = [1, 3] | params.tanque = 2 | sin parametros = los tres.
function codificarReporte(params) {
  var lista;
  if (params.tanques !== undefined) {
    lista = params.tanques;
  } else if (params.tanque !== undefined) {
    lista = [params.tanque];
  } else {
    lista = [1, 2, 3];
  }
  if (!Array.isArray(lista) || lista.length === 0) {
    return { errors: ["tanques tiene que ser una lista como [1, 3]"] };
  }

  var mascara = 0;
  for (var i = 0; i < lista.length; i++) {
    var t = lista[i];
    if (typeof t !== "number" || Math.floor(t) !== t || t < 1 || t > N_TANQUES) {
      return { errors: ["tanque invalido: " + t + " (tiene que ser 1, 2 o 3)"] };
    }
    mascara |= 1 << (t - 1);
  }
  return { bytes: [mascara], fPort: PUERTO_COMANDOS };
}

// ChirpStack no define "module": solo existe cuando se prueba el codec con Node.
if (typeof module !== "undefined") {
  module.exports = { decodeUplink: decodeUplink, encodeDownlink: encodeDownlink };
}
