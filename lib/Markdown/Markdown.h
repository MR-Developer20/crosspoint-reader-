#pragma once

#include <string>

// Converts a `.md` file into a synthetic EPUB in the book's cache directory, once, and lets
// the existing EPUB reader render it — mirrors lib/Jwpub's "convert once, reuse an existing
// reader" pattern (Jwpub -> content.txt -> TxtReaderActivity). Here the target is a real
// (if minimal) EPUB so Markdown gets full styled rendering — bold/italic, headings, lists,
// blockquotes, images, chapter navigation — instead of rendering as plain text.
//
// See lib/Markdown/MarkdownToXhtml.h for the Markdown subset understood, and
// lib/Markdown/StoredZipWriter.h for how the EPUB is packaged (uncompressed ZIP entries —
// miniz's archive-writing APIs are compiled out on this target, and no compression is
// needed since the source text is already small).
class Markdown {
  std::string filepath;       // the .md path
  std::string cacheBasePath;  // e.g. "/.crosspoint" — also the base the wrapping Epub caches under
  std::string cachePath;      // "<cacheBasePath>/md_<hash>"
  std::string epubPath;       // "<cachePath>/book.epub"
  bool loaded = false;

  [[nodiscard]] bool isCacheFresh() const;
  [[nodiscard]] bool convertToEpub() const;

 public:
  explicit Markdown(std::string path, std::string cacheBasePath);

  // Converts the .md to a synthetic EPUB if the cache is missing or stale (source file size
  // changed since the last conversion — the same staleness signal TxtReaderActivity's cache
  // uses, since the HAL exposes no file modification time), then reuses the cached EPUB
  // otherwise. Idempotent.
  bool load();

  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getEpubPath() const { return epubPath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }

  void setupCacheDir() const;
  bool clearCache() const;
};
