#pragma once

#include <string>

// Text-first JWPUB (JW Library publication) loader.
//
// `load()` converts a `.jwpub` into a single plaintext file (`content.txt`) in the book's
// cache directory: it extracts the nested SQLite database, then for every `Document` row
// decrypts + zlib-inflates the `Content` BLOB, strips the HTML to text, and concatenates
// the result. The existing TXT reader then renders `content.txt`. Conversion runs once;
// reopening is instant because `content.txt` is reused.
//
// This is the first (text-only) integration. A later phase feeds the decrypted HTML into
// the EPUB layout engine for styled text, images and a navigable TOC. Interface mirrors
// `lib/Txt/Txt.h` so it slots into the reader dispatch the same way.
class Jwpub {
  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;
  std::string title;
  bool loaded = false;

  // Full conversion: outer ZIP -> manifest -> key -> inner ZIP -> .db -> per-Document
  // decrypt/inflate/strip -> content.txt. Sets `title` from the manifest.
  bool convert();

 public:
  explicit Jwpub(std::string path, std::string cacheBasePath);

  bool load();
  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  // Path of the generated plaintext file the TXT reader renders.
  [[nodiscard]] std::string getContentPath() const { return cachePath + "/content.txt"; }
  [[nodiscard]] const std::string& getTitle() const { return title; }

  void setupCacheDir() const;
  bool clearCache() const;
};
