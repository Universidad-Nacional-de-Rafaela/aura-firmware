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
const SC = 0xffff;          // nivel: tanque sin calibrar
function reporte(ref, res, intervalo, ceros, llenos) {
  return [ref, res, ...be(intervalo, ...ceros, ...llenos)];
}

// ---------- subida: medicion (fPort 1) ----------
caso("un tanque sin calibrar trae solo la distancia en cm", () => {
  const r = decodeUplink({ fPort: 1, bytes: [0x02, ...be(2445, SC)] });
  assert.deepStrictEqual(r.data, { distancia_tanque2_cm: 244.5 });
  assert.strictEqual(r.errors, undefined);
});

caso("un tanque calibrado trae distancia y nivel", () => {
  const r = decodeUplink({ fPort: 1, bytes: [0x01, ...be(1007, 734)] });
  assert.deepStrictEqual(r.data, { distancia_tanque1_cm: 100.7, nivel_tanque1_pct: 73.4 });
});

caso("un ciclo: los tres tanques en un mensaje", () => {
  const r = decodeUplink({ fPort: 1, bytes: [0x07, ...be(2462, SC, 1500, 500, 300, 1000)] });
  assert.deepStrictEqual(r.data, {
    distancia_tanque1_cm: 246.2,
    distancia_tanque2_cm: 150, nivel_tanque2_pct: 50,
    distancia_tanque3_cm: 30, nivel_tanque3_pct: 100,
  });
});

caso("zona ciega (tanque lleno) se informa como 25 cm, no como falla", () => {
  const r = decodeUplink({ fPort: 1, bytes: [0x04, ...be(0xfffe, 1000)] });
  assert.deepStrictEqual(r.data, { distancia_tanque3_cm: 25, nivel_tanque3_pct: 100 });
  assert.strictEqual(r.errors, undefined);
});

caso("los bordes del rango (25 y 600 cm) son validos", () => {
  const r = decodeUplink({ fPort: 1, bytes: [0x03, ...be(250, 0, 6000, 0)] });
  assert.deepStrictEqual(r.data, {
    distancia_tanque1_cm: 25, nivel_tanque1_pct: 0, distancia_tanque2_cm: 600, nivel_tanque2_pct: 0,
  });
});

caso("sin eco, demasiado lejos o fuera de rango: el tanque no aparece (ni su nivel)", () => {
  const r = decodeUplink({ fPort: 1, bytes: [0x07, ...be(0xffff, 500, 1200, SC, 6001, 500)] });
  assert.deepStrictEqual(r.data, { distancia_tanque2_cm: 120 });
  assert.ok(r.warnings.some((w) => w.includes("tanque 1")));
});

caso("si ningun tanque es valido no hay data", () => {
  const r = decodeUplink({ fPort: 1, bytes: [0x01, ...be(0xfffd, SC)] });
  assert.strictEqual(r.data, undefined);
  assert.ok(r.errors[0].includes("sin lecturas validas"));
});

caso("mascara invalida o largo que no coincide es error", () => {
  assert.ok(decodeUplink({ fPort: 1, bytes: [0x00] }).errors);
  assert.ok(decodeUplink({ fPort: 1, bytes: [0x08, ...be(1000, SC)] }).errors);
  assert.ok(decodeUplink({ fPort: 1, bytes: [0x03, ...be(1000, SC)] }).errors[0].includes("se esperaban 9"));
  assert.ok(decodeUplink({ fPort: 1, bytes: [] }).errors);
});

caso("values solo trae distancia_tanqueN_cm y nivel_tanqueN_pct", () => {
  const r = decodeUplink({ fPort: 1, bytes: [0x07, ...be(1000, 100, 1000, SC, 1000, 999)] });
  for (const k of Object.keys(r.data)) assert.match(k, /^(distancia_tanque[123]_cm|nivel_tanque[123]_pct)$/);
});

caso("fPort desconocido es error", () => {
  assert.ok(decodeUplink({ fPort: 3, bytes: [1] }).errors);
});

// ---------- subida: reporte (fPort 2) ----------
caso("reporte aplicado sin calibrar", () => {
  const r = decodeUplink({ fPort: 2, bytes: reporte(7, 0, 300, [SC, SC, SC], [SC, SC, SC]) });
  assert.deepStrictEqual(r.data, { ref: 7, aplicado: true, config: { intervalo_s: 300 } });
});

caso("reporte con calibracion de un tanque", () => {
  const r = decodeUplink({ fPort: 2, bytes: reporte(9, 0, 60, [1600, SC, SC], [300, SC, SC]) });
  assert.deepStrictEqual(r.data, {
    ref: 9, aplicado: true, config: { intervalo_s: 60, cero_tanque1_cm: 160, lleno_tanque1_cm: 30 },
  });
});

caso("reporte rechazado con sus motivos", () => {
  const sc = [SC, SC, SC];
  assert.strictEqual(decodeUplink({ fPort: 2, bytes: reporte(1, 1, 60, sc, sc) }).data.motivo, "fuera_de_rango");
  assert.strictEqual(decodeUplink({ fPort: 2, bytes: reporte(1, 2, 60, sc, sc) }).data.motivo, "comando_desconocido");
  assert.strictEqual(decodeUplink({ fPort: 2, bytes: reporte(1, 3, 60, sc, sc) }).data.motivo, "calibracion_invalida");
  assert.strictEqual(decodeUplink({ fPort: 2, bytes: reporte(1, 9, 60, sc, sc) }).data.motivo, "rechazado_por_el_nodo");
  assert.strictEqual(decodeUplink({ fPort: 2, bytes: reporte(1, 1, 60, sc, sc) }).data.aplicado, false);
});

caso("reporte con largo incorrecto es error", () => {
  assert.ok(decodeUplink({ fPort: 2, bytes: [1, 0, 0, 60] }).errors);
});

// ---------- bajada ----------
caso("medir_ahora", () => {
  assert.deepStrictEqual(encodeDownlink({ data: { command: "medir_ahora", params: {}, ref: 5 } }),
    { bytes: [0x01, 5] });
});

caso("set_config solo intervalo_s: forma corta de 4 bytes", () => {
  const r = encodeDownlink({ data: { command: "set_config", params: { intervalo_s: 300 }, ref: 7 } });
  assert.deepStrictEqual(r, { bytes: [0x02, 7, 0x01, 0x2c] });
});

caso("set_config intervalo_s con calibracion: entradas id hi lo", () => {
  const r = encodeDownlink({ data: { command: "set_config", ref: 7,
    params: { intervalo_s: 300, cero_tanque1_cm: 160 } } });
  assert.deepStrictEqual(r, { bytes: [0x02, 7, 0x01, 0x01, 0x2c, 0x11, ...be(1600)] });
});

caso("set_config cero y lleno de un tanque, en cm con un decimal", () => {
  const r = encodeDownlink({ data: { command: "set_config", ref: 8,
    params: { cero_tanque2_cm: 160.5, lleno_tanque2_cm: 30 } } });
  assert.deepStrictEqual(r, { bytes: [0x02, 8, 0x12, ...be(1605), 0x22, ...be(300)] });
});

caso("set_config con todo junto (7 parametros, 23 bytes)", () => {
  const params = { intervalo_s: 120 };
  for (const t of [1, 2, 3]) { params[`cero_tanque${t}_cm`] = 160; params[`lleno_tanque${t}_cm`] = 30; }
  const r = encodeDownlink({ data: { command: "set_config", params, ref: 1 } });
  assert.strictEqual(r.bytes.length, 23);
});

caso("set_config en los bordes", () => {
  assert.deepStrictEqual(encodeDownlink({ data: { command: "set_config", params: { intervalo_s: 43200 }, ref: 255 } }).bytes,
    [2, 255, 0xa8, 0xc0]);
  assert.deepStrictEqual(encodeDownlink({ data: { command: "set_config", params: { intervalo_s: 60 }, ref: 1 } }).bytes,
    [2, 1, 0, 60]);
  assert.deepStrictEqual(encodeDownlink({ data: { command: "set_config", params: { cero_tanque3_cm: 600 }, ref: 1 } }).bytes,
    [2, 1, 0x13, ...be(6000)]);
  assert.deepStrictEqual(encodeDownlink({ data: { command: "set_config", params: { lleno_tanque1_cm: 25 }, ref: 1 } }).bytes,
    [2, 1, 0x21, ...be(250)]);
});

caso("set_config invalido no codifica nada", () => {
  for (const params of [{}, { intervalo_s: 59 }, { intervalo_s: 43201 }, { intervalo_s: 90.5 },
                        { intervalo_s: "300" }, { umbral: 1 }, { cero_tanque4_cm: 100 },
                        { cero_tanque1_cm: 24.9 }, { lleno_tanque1_cm: 600.1 }, { cero_tanque1_cm: 100.25 },
                        { cero_tanque1_cm: "160" }, { cero_tanque1_cm: 35, lleno_tanque1_cm: 30 }]) {
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

console.log(corridos + " casos OK");
