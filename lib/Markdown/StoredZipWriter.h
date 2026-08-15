#pragma once

#include <HalStorage.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Minimal ZIP writer producing STORED (uncompressed) entries only — no deflate. Used to
// package a converted Markdown document as a synthetic EPUB, since EPUB is just a ZIP with
// a fixed internal layout and the reader (lib/ZipFile) already reads stored entries fine
// (ZipFile.cpp handles ZIP_METHOD_STORED). miniz is built with archive-writing APIs
// disabled on this target (see lib/miniz/src/MinizConfig.h), so a full writer isn't
// available; this class only needs mz_crc32(), which stays exported.
//
// Every entry's CRC-32 and size are known before its local header is written (buffer
// entries carry them already; file entries are pre-scanned once), so no ZIP64 / data
// descriptor support is needed and every entry is fully spec-compliant for any reader,
// including this firmware's own ZipFile (lib/ZipFile/ZipFile.cpp), which resolves sizes
// from the central directory only.
class StoredZipWriter {
 public:
  // `out` must already be open for write, positioned at offset 0. `expectedEntries` is a
  // capacity hint for the in-RAM entry table (reserve, not a hard limit).
  explicit StoredZipWriter(HalFile& out, size_t expectedEntries = 8);

  // Add an entry from an in-memory buffer (small generated files: mimetype, OPF, NCX, CSS).
  bool addBufferEntry(std::string_view zipEntryName, const uint8_t* data, size_t len);

  // Add an entry by streaming the full contents of an existing SD file. Reads the source
  // once to compute CRC-32 and confirm size, then reads it again to copy into the archive.
  bool addFileEntry(std::string_view zipEntryName, const std::string& srcPath);

  // Same, but the caller already knows the CRC-32 and size (e.g. it just streamed the file
  // out itself and accumulated a running CRC) — skips the pre-scan read pass entirely.
  bool addFileEntryWithKnownCrc(std::string_view zipEntryName, const std::string& srcPath, uint32_t crc32,
                                uint32_t size);

  // Writes the central directory + End Of Central Directory record. Call once, after every
  // entry has been added. No further entries may be added afterward.
  bool finish();

 private:
  struct EntryRecord {
    std::string name;
    uint32_t crc = 0;
    uint32_t size = 0;  // == compressed size, since method is always STORED
    uint32_t localHeaderOffset = 0;
  };

  static constexpr size_t IO_BUFFER_SIZE = 8192;

  HalFile& out_;
  std::vector<EntryRecord> entries_;
  std::unique_ptr<uint8_t[]> ioBuffer_;
  bool failed_ = false;

  bool ensureBuffer();
  bool writeLocalHeader(std::string_view name, uint32_t crc, uint32_t size, uint32_t* outHeaderOffset);
  bool writeAll(const uint8_t* data, size_t len);
};
