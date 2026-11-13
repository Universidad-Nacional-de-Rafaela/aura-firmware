#include "aserciones.h"
#include "../aes_cmac.h"
#include <string>
#include <vector>

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

static void test_sbox() {
  aes_preparar_tablas();
  VERIFICAR(aes_sbox[0x00] == 0x63);
  VERIFICAR(aes_sbox[0x01] == 0x7c);
  VERIFICAR(aes_sbox[0x53] == 0xed);
  VERIFICAR(aes_sbox[0xff] == 0x16);
  for (int i = 0; i < 256; i++) VERIFICAR(aes_inv_sbox[aes_sbox[i]] == i);
}

static void test_fips197() {
  // Apendice B
  auto k = hex("2b7e151628aed2a6abf7158809cf4f3c");
  auto p = hex("3243f6a8885a308d313198a2e0370734");
  aes128_ctx c;
  aes128_iniciar(&c, k.data());
  uint8_t o[16], d[16];
  aes128_cifrar_bloque(&c, p.data(), o);
  VERIFICAR(igual(o, "3925841d02dc09fbdc118597196a0b32"));
  aes128_descifrar_bloque(&c, o, d);
  VERIFICAR(memcmp(d, p.data(), 16) == 0);
  // Apendice C.1
  auto k2 = hex("000102030405060708090a0b0c0d0e0f");
  auto p2 = hex("00112233445566778899aabbccddeeff");
  aes128_iniciar(&c, k2.data());
  aes128_cifrar_bloque(&c, p2.data(), o);
  VERIFICAR(igual(o, "69c4e0d86a7b0430d8cdb78070b4c55a"));
  aes128_descifrar_bloque(&c, o, d);
  VERIFICAR(memcmp(d, p2.data(), 16) == 0);
  // clave de fabrica en ceros, bloque en ceros (vector conocido de AES-128)
  uint8_t z[16] = {0};
  aes128_iniciar(&c, z);
  aes128_cifrar_bloque(&c, z, o);
  VERIFICAR(igual(o, "66e94bd4ef8a2c3b884cfa59ca342b2e"));
}

static void test_cbc() {
  // NIST SP 800-38A F.2.1 (CBC-AES128), con su IV
  auto k = hex("2b7e151628aed2a6abf7158809cf4f3c");
  auto iv = hex("000102030405060708090a0b0c0d0e0f");
  auto p = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
  aes128_ctx c;
  aes128_iniciar(&c, k.data());
  uint8_t o[32], d[32];
  aes128_cbc_cifrar(&c, iv.data(), p.data(), o, 32);
  VERIFICAR(igual(o, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"));
  aes128_cbc_descifrar(&c, iv.data(), o, d, 32);
  VERIFICAR(memcmp(d, p.data(), 32) == 0);
  // en el lugar
  uint8_t e[32];
  memcpy(e, p.data(), 32);
  aes128_cbc_cifrar(&c, iv.data(), e, e, 32);
  VERIFICAR(memcmp(e, o, 32) == 0);
  aes128_cbc_descifrar(&c, iv.data(), e, e, 32);
  VERIFICAR(memcmp(e, p.data(), 32) == 0);
}

static void test_cmac_rfc4493() {
  auto k = hex("2b7e151628aed2a6abf7158809cf4f3c");
  aes128_ctx c;
  aes128_iniciar(&c, k.data());
  uint8_t k1[16], k2[16];
  aes_cmac_subclaves(&c, k1, k2);
  VERIFICAR(igual(k1, "fbeed618357133667c85e08f7236a8de"));
  VERIFICAR(igual(k2, "f7ddac306ae266ccf90bc11ee46d513b"));

  auto m = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
               "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
  uint8_t mac[16];
  aes_cmac(k.data(), m.data(), 0, mac);
  VERIFICAR(igual(mac, "bb1d6929e95937287fa37d129b756746"));
  aes_cmac(k.data(), m.data(), 16, mac);
  VERIFICAR(igual(mac, "070a16b46b4d4144f79bdd9dd04a287c"));
  aes_cmac(k.data(), m.data(), 40, mac);
  VERIFICAR(igual(mac, "dfa66747de9ae63030ca32611497c827"));
  aes_cmac(k.data(), m.data(), 64, mac);
  VERIFICAR(igual(mac, "51f0bebf7e3b9d92fc49741779363cfe"));
}

int main() {
  test_sbox();
  test_fips197();
  test_cbc();
  test_cmac_rfc4493();
  RESUMEN();
}
