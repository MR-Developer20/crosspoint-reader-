#include "JwpubCrypto.h"

#include <Logging.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

namespace {
// Fixed 32-byte constant XORed into SHA-256(keyMaterial) during key derivation.
// Hex: 11cbb5587e32846d4c26790c633da289f66fe5842a3a585ce1bc3a294af5ada7
constexpr uint8_t MASTER_XOR[JwpubCrypto::KEY_IV_SIZE] = {
    0x11, 0xcb, 0xb5, 0x58, 0x7e, 0x32, 0x84, 0x6d, 0x4c, 0x26, 0x79, 0x0c, 0x63, 0x3d, 0xa2, 0x89,
    0xf6, 0x6f, 0xe5, 0x84, 0x2a, 0x3a, 0x58, 0x5c, 0xe1, 0xbc, 0x3a, 0x29, 0x4a, 0xf5, 0xad, 0xa7};
}  // namespace

size_t JwpubCrypto::buildKeyMaterial(int mepsLanguageIndex, const std::string& symbol, int year, int issueTagNumber,
                                     char* out, size_t outSize) {
  int n;
  if (issueTagNumber != 0) {
    n = snprintf(out, outSize, "%d_%s_%d_%d", mepsLanguageIndex, symbol.c_str(), year, issueTagNumber);
  } else {
    n = snprintf(out, outSize, "%d_%s_%d", mepsLanguageIndex, symbol.c_str(), year);
  }
  if (n < 0 || static_cast<size_t>(n) >= outSize) {
    return 0;  // encoding error or truncation
  }
  return static_cast<size_t>(n);
}

bool JwpubCrypto::deriveKeyIv(int mepsLanguageIndex, const std::string& symbol, int year, int issueTagNumber,
                              uint8_t keyIv[KEY_IV_SIZE]) {
  char material[128];
  const size_t len = buildKeyMaterial(mepsLanguageIndex, symbol, year, issueTagNumber, material, sizeof(material));
  if (len == 0) {
    LOG_ERR("JWCRYPTO", "key material build failed (symbol=%s)", symbol.c_str());
    return false;
  }

  // SHA-256 of the key material. Matches the non-_ret mbedtls call style used elsewhere in
  // the tree (FirmwareFlasher); a hash over a tiny in-RAM buffer does not fail in practice.
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, /*is224=*/0);
  mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(material), len);
  mbedtls_sha256_finish(&ctx, keyIv);
  mbedtls_sha256_free(&ctx);

  for (size_t i = 0; i < KEY_IV_SIZE; i++) {
    keyIv[i] ^= MASTER_XOR[i];
  }
  return true;
}

bool JwpubCrypto::decryptInPlace(const uint8_t keyIv[KEY_IV_SIZE], uint8_t* data, size_t len, size_t* outLen) {
  if (len == 0 || (len % 16) != 0) {
    LOG_ERR("JWCRYPTO", "bad ciphertext length: %zu", len);
    return false;
  }

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (mbedtls_aes_setkey_dec(&ctx, keyIv, 128) != 0) {
    mbedtls_aes_free(&ctx);
    LOG_ERR("JWCRYPTO", "aes setkey failed");
    return false;
  }

  // mbedtls rewrites the IV buffer as it runs, so decrypt from a copy and leave keyIv intact.
  uint8_t iv[IV_SIZE];
  memcpy(iv, keyIv + KEY_SIZE, IV_SIZE);

  const int rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, len, iv, data, data);
  mbedtls_aes_free(&ctx);
  if (rc != 0) {
    LOG_ERR("JWCRYPTO", "aes cbc failed: %d", rc);
    return false;
  }

  // Strip PKCS#7 padding: the final byte is the pad length (1..16) and every padding byte
  // must equal it.
  const uint8_t pad = data[len - 1];
  if (pad == 0 || pad > 16 || static_cast<size_t>(pad) > len) {
    LOG_ERR("JWCRYPTO", "bad pkcs7 padding: %u", pad);
    return false;
  }
  for (size_t i = len - pad; i < len; i++) {
    if (data[i] != pad) {
      LOG_ERR("JWCRYPTO", "inconsistent pkcs7 padding");
      return false;
    }
  }
  *outLen = len - pad;
  return true;
}
