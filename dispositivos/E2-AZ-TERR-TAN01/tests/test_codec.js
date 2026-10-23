// Tests del codec de ChirpStack. Correr con: node test_codec.js (o make, en esta carpeta).
const assert = require("assert");
const path = require("path");
const { decodeUplink, encodeDownlink } = require(path.join(__dirname, "..", "codec.js"));

let corridos = 0;
function caso(nombre, fn) {
  fn();
  corridos++;
  console.log("ok   " + nombre);
}
function be(...valores) {   // uint16 big endian
  const b = [];
  for (const v of valores) b.push((v >> 8) & 0xff, v & 0xff);
  return b;
}

// ---------- subida: medicion (fPort 1) ----------
caso("medicion con los tres tanques", () => {
  const r = decodeUplink({ fPort: 1, bytes: be(1500, 2000, 3000) });
  assert.deepStrictEqual(r.data, {
    distancia_tanque1_cm: 150, distancia_tanque2_cm: 200, distancia_tanque3_cm: 300,
  });
  assert.strictEqual(r.errors, undefined);
});

caso("los mm de los bytes pasan a cm con un decimal", () => {
  const r = decodeUplink({ fPort: 1, bytes: be(2462, 2445, 1007) });
  assert.deepStrictEqual(r.data, {
    distancia_tanque1_cm: 246.2, distancia_tanque2_cm: 244.5, distancia_tanque3_cm: 100.7,
  });
});

caso("un sensor sin eco no aparece en values", () => {
  const r = decodeUplink({ fPort: 1, bytes: be(1510, 0xffff, 4500) });
  assert.deepStrictEqual(r.data, { distancia_tanque1_cm: 151, distancia_tanque3_cm: 450 });
  assert.ok(r.warnings[0].includes("tanque 2"));
});

caso("sin eco, demasiado lejos y fuera de rango no aparecen", () => {
  const r = decodeUplink({ fPort: 1, bytes: be(0xffff, 0xfffd, 249) });
  assert.strictEqual(r.data, undefined);
  assert.ok(r.errors[0].includes("sin lecturas validas"));
});

caso("zona ciega (tanque lleno) se informa como 25 cm, no como falla", () => {
  const r = decodeUplink({ fPort: 1, bytes: be(0xfffe, 1200, 0xffff) });
  assert.deepStrictEqual(r.data, { distancia_tanque1_cm: 25, distancia_tanque2_cm: 120 });
  assert.strictEqual(r.errors, undefined);
});

caso("los tres tanques llenos publican data (no es error)", () => {
  const r = decodeUplink({ fPort: 1, bytes: be(0xfffe, 0xfffe, 0xfffe) });
  assert.deepStrictEqual(r.data, {
    distancia_tanque1_cm: 25, distancia_tanque2_cm: 25, distancia_tanque3_cm: 25,
  });
  assert.strictEqual(r.errors, undefined);
});

caso("los bordes del rango (25 y 600 cm) son validos", () => {
  const r = decodeUplink({ fPort: 1, bytes: be(250, 6000, 6001) });
  assert.deepStrictEqual(r.data, { distancia_tanque1_cm: 25, distancia_tanque2_cm: 600 });
});

caso("medicion con largo incorrecto es error", () => {
  const r = decodeUplink({ fPort: 1, bytes: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0] });
  assert.ok(r.errors[0].includes("se esperaban 6"));
});

caso("fPort desconocido es error", () => {
  assert.ok(decodeUplink({ fPort: 3, bytes: [1] }).errors);
});

caso("values no trae campos de diagnostico", () => {
  const r = decodeUplink({ fPort: 1, bytes: be(1000, 0xffff, 2000) });
  for (const k of Object.keys(r.data)) assert.match(k, /^distancia_tanque[123]_cm$/);
});

// ---------- subida: reporte (fPort 2) ----------
caso("reporte aplicado", () => {
  const r = decodeUplink({ fPort: 2, bytes: [7, 0, ...be(300)] });
  assert.deepStrictEqual(r.data, { ref: 7, aplicado: true, config: { intervalo_s: 300 } });
});

caso("reporte fuera de rango", () => {
  const r = decodeUplink({ fPort: 2, bytes: [8, 1, ...be(60)] });
  assert.deepStrictEqual(r.data,
    { ref: 8, aplicado: false, config: { intervalo_s: 60 }, motivo: "fuera_de_rango" });
});

caso("reporte comando desconocido y resultado sin nombre", () => {
  assert.strictEqual(decodeUplink({ fPort: 2, bytes: [9, 2, 0, 60] }).data.motivo, "comando_desconocido");
  assert.strictEqual(decodeUplink({ fPort: 2, bytes: [9, 5, 0, 60] }).data.motivo, "rechazado_por_el_nodo");
});

caso("reporte con largo incorrecto es error", () => {
  assert.ok(decodeUplink({ fPort: 2, bytes: [1, 0, 0] }).errors);
});

// ---------- bajada ----------
caso("medir_ahora", () => {
  assert.deepStrictEqual(encodeDownlink({ data: { command: "medir_ahora", params: {}, ref: 5 } }),
    { bytes: [0x01, 5] });
});

caso("set_config 300", () => {
  const r = encodeDownlink({ data: { command: "set_config", params: { intervalo_s: 300 }, ref: 7 } });
  assert.deepStrictEqual(r, { bytes: [0x02, 7, 0x01, 0x2c] });
});

caso("set_config en los bordes (60 y 43200)", () => {
  assert.deepStrictEqual(encodeDownlink({ data: { command: "set_config", params: { intervalo_s: 60 }, ref: 1 } }).bytes,
    [2, 1, 0, 60]);
  assert.deepStrictEqual(encodeDownlink({ data: { command: "set_config", params: { intervalo_s: 43200 }, ref: 255 } }).bytes,
    [2, 255, 0xa8, 0xc0]);
});

caso("set_config acepta cualquier entero del rango (90 s: turnos de 30 s)", () => {
  const r = encodeDownlink({ data: { command: "set_config", params: { intervalo_s: 90 }, ref: 3 } });
  assert.deepStrictEqual(r, { bytes: [2, 3, 0, 90] });
});

caso("set_config fuera de rango, no entero o con otro parametro es error", () => {
  for (const params of [{ intervalo_s: 15 }, { intervalo_s: 43201 }, { intervalo_s: 90.5 },
                        { intervalo_s: "300" }, {}, { intervalo_s: 300, umbral: 1 }]) {
    const r = encodeDownlink({ data: { command: "set_config", params, ref: 2 } });
    assert.ok(r.errors, JSON.stringify(params));
    assert.strictEqual(r.bytes, undefined);
  }
});

caso("sin ref valido es error", () => {
  for (const ref of [undefined, 0, 256, 1.5]) {
    assert.ok(encodeDownlink({ data: { command: "medir_ahora", ref } }).errors, String(ref));
  }
});

caso("comando desconocido es error", () => {
  assert.ok(encodeDownlink({ data: { command: "medir_hora", ref: 1 } }).errors);
});

// ---------- ida y vuelta: lo que codifica la bajada, el reporte lo devuelve ----------
caso("set_config -> reporte con el mismo ref e intervalo", () => {
  const bajada = encodeDownlink({ data: { command: "set_config", params: { intervalo_s: 600 }, ref: 42 } }).bytes;
  const reporte = [bajada[1], 0, bajada[2], bajada[3]];   // lo que arma el nodo si lo aplica
  assert.deepStrictEqual(decodeUplink({ fPort: 2, bytes: reporte }).data,
    { ref: 42, aplicado: true, config: { intervalo_s: 600 } });
});

console.log(corridos + " casos OK");
