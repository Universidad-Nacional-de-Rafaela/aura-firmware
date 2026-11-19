#include "aserciones.h"
#include "../sha256_hmac.h"
#include <vector>
#include <cstdlib>

static std::vector<uint8_t> hex(const char *s) {
  std::vector<uint8_t> v;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
    char b[3] = {s[i], s[i + 1], 0};
    v.push_back((uint8_t)strtoul(b, nullptr, 16));
  }
  return v;
}
static bool igual(const uint8_t *a, const char *h) {
  auto v = hex(h);
  return memcmp(a, v.data(), v.size()) == 0;
}

static void sha(const char *m, size_t n, uint8_t out[32]) {
  sha256_ctx c;
  sha256_iniciar(&c);
  sha256_agregar(&c, (const uint8_t *)m, n);
  sha256_fin(&c, out);
}

static void test_sha256() {
  uint8_t o[32];
  sha("", 0, o);
  VERIFICAR(igual(o, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  sha("abc", 3, o);
  VERIFICAR(igual(o, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  sha(m, strlen(m), o);
  VERIFICAR(igual(o, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  // en pedazos == de una
  sha256_ctx c;
  sha256_iniciar(&c);
  for (size_t i = 0; i < strlen(m); i++) sha256_agregar(&c, (const uint8_t *)&m[i], 1);
  sha256_fin(&c, o);
  VERIFICAR(igual(o, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

static void test_hmac_rfc4231() {
  uint8_t o[32];
  // caso 1
  auto k1 = hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
  hmac_sha256(k1.data(), k1.size(), (const uint8_t *)"Hi There", 8, o);
  VERIFICAR(igual(o, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
  // caso 2
  hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, o);
  VERIFICAR(igual(o, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
  // caso 6: clave de 131 bytes (mas larga que el bloque)
  std::vector<uint8_t> k6(131, 0xaa);
  const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
  hmac_sha256(k6.data(), k6.size(), (const uint8_t *)m6, strlen(m6), o);
  VERIFICAR(igual(o, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

static void test_clave_de_tarjeta() {
  uint8_t maestra[32], a[16], b[16], c[16];
  for (int i = 0; i < 32; i++) maestra[i] = (uint8_t)i;
  const uint8_t uid1[7] = {0x04, 0x45, 0x65, 0xBA, 0x77, 0x23, 0x90};
  const uint8_t uid2[7] = {0x04, 0x45, 0x66, 0xBA, 0x77, 0x23, 0x90};
  VERIFICAR(clave_de_tarjeta(maestra, uid1, 7, a) == 0);
  VERIFICAR(clave_de_tarjeta(maestra, uid1, 7, b) == 0);
  VERIFICAR(memcmp(a, b, 16) == 0);                         // determinista
  VERIFICAR(clave_de_tarjeta(maestra, uid2, 7, c) == 0);
  VERIFICAR(memcmp(a, c, 16) != 0);                         // cada tarjeta, su clave
  maestra[0] ^= 1;
  VERIFICAR(clave_de_tarjeta(maestra, uid1, 7, c) == 0);
  VERIFICAR(memcmp(a, c, 16) != 0);                         // otra maestra, otra clave
  VERIFICAR(clave_de_tarjeta(maestra, uid1, 5, c) == -1);   // largo de UID invalido
  VERIFICAR(clave_de_tarjeta(maestra, uid1, 0, c) == -1);
  // es el prefijo de 16 bytes del HMAC completo
  uint8_t h[32];
  maestra[0] ^= 1;
  hmac_sha256(maestra, 32, uid1, 7, h);
  VERIFICAR(memcmp(a, h, 16) == 0);
}

int main() {
  test_sha256();
  test_hmac_rfc4231();
  test_clave_de_tarjeta();
  RESUMEN();
}
