#pragma once

#include <string>

// Parsed subset of a JWPUB manifest.json.
//
// manifest.json is the plaintext metadata file at the root of a .jwpub archive. Only the
// fields needed to (a) derive the content-decryption key and (b) show basic book metadata
// are extracted; the large `images` / table-of-contents arrays are filtered out so the
// backing JSON document stays small.
//
// NOTE: the exact publication-card field names must be confirmed against a real .jwpub
// during bring-up (see the plan's Phase 1 verification). They are all read in one place
// (parse()) so they are easy to correct. In particular the MEPS language index has been
// seen as publication.language.
struct JwpubManifest {
  // Top-level fields.
  std::string name;
  std::string hash;
  std::string contentFormat;  // expected "z-a" (zlib then AES)

  // Publication card — the key-derivation inputs plus display metadata.
  std::string title;
  std::string symbol;
  int mepsLanguageIndex = -1;  // >= 0 when present (English is 0)
  int year = 0;
  int issueTagNumber = 0;

  // True when the fields required for key derivation are present.
  bool hasKeyMaterial() const { return !symbol.empty() && mepsLanguageIndex >= 0; }

  // True when the content is zlib-then-AES ("z-a"); other formats are unsupported.
  bool isEncryptedZlib() const { return contentFormat == "z-a"; }

  // Parse a null-terminated manifest.json buffer. Returns true on a successful JSON parse
  // (check hasKeyMaterial()/isEncryptedZlib() for semantic validity).
  bool parse(const char* json);
};
