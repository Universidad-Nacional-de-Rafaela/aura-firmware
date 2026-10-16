// Codec de ChirpStack v4 para E1-AZ-TERR-TAN01 (nivel de dos tanques de agua).
// Se carga en el device profile del dispositivo. Lo que devuelve decodeUplink es
// el "object" del uplink, que el lorawan-bridge publica tal cual como "values" en
// hw/eui-<DevEUI>/data (contrato v4.0, §3.1).
//
// Payload de 4 bytes, big endian:
//
//   byte  0-1  distancia del sensor al agua, tanque 1, en cm x 10 (uint16)
//   byte  2-3  lo mismo para el tanque 2
//
// El nodo manda solo la distancia: el porcentaje y los litros los calcula el
// conector en AURA, que conoce las medidas de cada tanque.
//
// Sin lectura: si el firmware no pudo medir, manda 0xFFFF. Ese campo no aparece
// en values: el contrato no admite valores centinela (§3.1). Si no queda ningun
// campo, el uplink sale con error y el bridge no publica data.
//
// Los nombres de los campos llevan la unidad y tienen que coincidir con el
// manifiesto del conector (conectores/E1-AZ-TERR-TAN01/conector.toml).

var SIN_LECTURA = 0xFFFF;

function u16(bytes, i) {
  return (bytes[i] << 8) | bytes[i + 1];
}

function decodeUplink(input) {
  var bytes = input.bytes;
  if (bytes.length !== 4) {
    return { errors: ["largo de payload invalido: " + bytes.length + " bytes, se esperaban 4"] };
  }

  var values = {};
  for (var n = 1; n <= 2; n++) {
    var dist = u16(bytes, 2 * (n - 1));
    if (dist !== SIN_LECTURA) values["dist_t" + n + "_cm"] = dist / 10.0;
  }

  if (Object.keys(values).length === 0) {
    return { errors: ["sin lecturas validas en ningun tanque"] };
  }
  return { data: values };
}

// Comandos de bajada (contrato §3.3). El lorawan-bridge encola el JSON del
// comando como "object" y esto lo pasa a bytes. El nodo es clase C: el comando
// llega en segundos, sin esperar al proximo uplink.
//
//   {"command": "medir_ahora"}  ->  [0x01]   mide y envia sin esperar al intervalo
//
// El nodo identifica el comando por el contenido, no por el fPort; se usa el 10.
// Cualquier otro comando sale con error y no se encola.

var COMANDOS = { medir_ahora: 0x01 };

function encodeDownlink(input) {
  var cmd = input.data && input.data.command;
  if (!COMANDOS.hasOwnProperty(cmd)) {
    return { errors: ["comando desconocido: " + cmd] };
  }
  return { bytes: [COMANDOS[cmd]] };
}
