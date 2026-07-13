#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// JWPUB content decryption.
//
// A JWPUB `Document.Content` blob is zlib-compressed and THEN AES-128-CBC encrypted
// (the manifest flags this as contentFormat "z-a"). So the read order is:
//   ciphertext --AES-128-CBC decrypt--> PKCS#7 unpad --> zlib stream --inflate--> UTF-8 HTML.
// This class covers the AES half; inflation is done by the caller with the existing
// uzlib/InflateReader.
//
// The key/IV come entirely from the PLAINTEXT manifest.json publication card, so no data
// from the encrypted database is needed to bootstrap decryption. Derivation (matches the
// reference implementation gokusander/jwpub-toolkit, MIT):
//   1. s = "<mepsLanguageIndex>_<symbol>_<year>", append "_<issueTagNumber>" iff it is != 0
//   2. h = SHA-256(s)                         -> 32 bytes
//   3. h[i] ^= MASTER_XOR[i]                   (fixed 32-byte constant, below)
//   4. key = h[0..15], iv = h[16..31]         (AES-128-CBC)
//
// NOTE: the exact composition of the key-material string (step 1) must be validated
// against a real sample during bring-up; it is centralised in buildKeyMaterial() so it is
// easy to adjust if a publication variant differs.
class JwpubCrypto {
 public:
  static constexpr size_t KEY_SIZE = 16;
  static constexpr size_t IV_SIZE = 16;
  static constexpr size_t KEY_IV_SIZE = KEY_SIZE + IV_SIZE;  // 32 (== SHA-256 output)

  // Build the "<meps>_<symbol>_<year>[_<issue>]" key-material string into `out`.
  // Returns the string length (excluding the null terminator), or 0 on failure.
  static size_t buildKeyMaterial(int mepsLanguageIndex, const std::string& symbol, int year, int issueTagNumber,
                                 char* out, size_t outSize);

  // Derive key||iv (32 bytes) from the publication card fields. Returns true on success.
  static bool deriveKeyIv(int mepsLanguageIndex, const std::string& symbol, int year, int issueTagNumber,
                          uint8_t keyIv[KEY_IV_SIZE]);

  // Decrypt an AES-128-CBC blob in place. `len` must be a non-zero multiple of 16.
  // On success strips the PKCS#7 padding and writes the unpadded length to *outLen.
  // `keyIv` is key||iv as produced by deriveKeyIv(). Returns false on bad length,
  // an mbedtls error, or invalid padding.
  static bool decryptInPlace(const uint8_t keyIv[KEY_IV_SIZE], uint8_t* data, size_t len, size_t* outLen);
};
