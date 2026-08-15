#include "StoredZipWriter.h"

#include <Logging.h>
#include <Memory.h>
#include <MinizConfig.h>

namespace {
constexpr uint32_t LOCAL_HEADER_SIG = 0x04034b50;
constexpr uint32_t CENTRAL_HEADER_SIG = 0x02014b50;
constexpr uint32_t EOCD_SIG = 0x06054b50;
constexpr uint16_t VERSION_NEEDED = 20;
constexpr uint16_t METHOD_STORED = 0;
// Fixed DOS date/time (1980-01-01 00:00:00) — EPUB readers don't use entry mtimes, and a
// constant value avoids depending on wall-clock time being set on the device.
constexpr uint16_t DOS_TIME = 0;
constexpr uint16_t DOS_DATE = 0x0021;
}  // namespace

StoredZipWriter::StoredZipWriter(HalFile& out, size_t expectedEntries) : out_(out) {
  entries_.reserve(expectedEntries);
}

bool StoredZipWriter::ensureBuffer() {
  if (ioBuffer_) {
    return true;
  }
  ioBuffer_ = makeUniqueNoThrow<uint8_t[]>(IO_BUFFER_SIZE);
  if (!ioBuffer_) {
    LOG_ERR("MDZ", "OOM: %d byte IO buffer", static_cast<int>(IO_BUFFER_SIZE));
    failed_ = true;
  }
  return static_cast<bool>(ioBuffer_);
}

bool StoredZipWriter::writeAll(const uint8_t* data, size_t len) { return out_.write(data, len) == len; }

bool StoredZipWriter::writeLocalHeader(std::string_view name, uint32_t crc, uint32_t size, uint32_t* outHeaderOffset) {
  *outHeaderOffset = static_cast<uint32_t>(out_.position());

  bool ok = true;
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&LOCAL_HEADER_SIG), sizeof(LOCAL_HEADER_SIG));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&VERSION_NEEDED), sizeof(VERSION_NEEDED));
  constexpr uint16_t generalPurposeFlag = 0;
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&generalPurposeFlag), sizeof(generalPurposeFlag));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&METHOD_STORED), sizeof(METHOD_STORED));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&DOS_TIME), sizeof(DOS_TIME));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&DOS_DATE), sizeof(DOS_DATE));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&crc), sizeof(crc));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&size), sizeof(size));  // compressed size
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&size), sizeof(size));  // uncompressed size (== compressed, stored)
  const auto nameLen = static_cast<uint16_t>(name.size());
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&nameLen), sizeof(nameLen));
  constexpr uint16_t extraLen = 0;
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&extraLen), sizeof(extraLen));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  return ok;
}

bool StoredZipWriter::addBufferEntry(std::string_view zipEntryName, const uint8_t* data, size_t len) {
  if (failed_) {
    return false;
  }
  const uint32_t crc = mz_crc32(MZ_CRC32_INIT, data, len);
  const auto size = static_cast<uint32_t>(len);

  uint32_t headerOffset;
  if (!writeLocalHeader(zipEntryName, crc, size, &headerOffset) || !writeAll(data, len)) {
    LOG_ERR("MDZ", "Failed writing buffer entry '%.*s'", static_cast<int>(zipEntryName.size()), zipEntryName.data());
    failed_ = true;
    return false;
  }

  entries_.push_back({std::string(zipEntryName), crc, size, headerOffset});
  return true;
}

bool StoredZipWriter::addFileEntry(std::string_view zipEntryName, const std::string& srcPath) {
  if (failed_ || !ensureBuffer()) {
    return false;
  }

  HalFile src;
  if (!Storage.openFileForRead("MDZ", srcPath, src)) {
    LOG_ERR("MDZ", "Cannot open source file for CRC scan: %s", srcPath.c_str());
    return false;
  }

  uint32_t crc = MZ_CRC32_INIT;
  const auto size = static_cast<uint32_t>(src.size());
  uint32_t remaining = size;
  while (remaining > 0) {
    const size_t chunk = remaining < IO_BUFFER_SIZE ? remaining : IO_BUFFER_SIZE;
    const int readBytes = src.read(ioBuffer_.get(), chunk);
    if (readBytes <= 0 || static_cast<size_t>(readBytes) != chunk) {
      LOG_ERR("MDZ", "Short read scanning '%s' for CRC", srcPath.c_str());
      return false;
    }
    crc = mz_crc32(crc, ioBuffer_.get(), chunk);
    remaining -= static_cast<uint32_t>(chunk);
  }

  return addFileEntryWithKnownCrc(zipEntryName, srcPath, crc, size);
}

bool StoredZipWriter::addFileEntryWithKnownCrc(std::string_view zipEntryName, const std::string& srcPath,
                                               uint32_t crc32, uint32_t size) {
  if (failed_ || !ensureBuffer()) {
    return false;
  }

  HalFile src;
  if (!Storage.openFileForRead("MDZ", srcPath, src)) {
    LOG_ERR("MDZ", "Cannot open source file to copy: %s", srcPath.c_str());
    return false;
  }

  uint32_t headerOffset;
  if (!writeLocalHeader(zipEntryName, crc32, size, &headerOffset)) {
    LOG_ERR("MDZ", "Failed writing local header for '%s'", srcPath.c_str());
    failed_ = true;
    return false;
  }

  uint32_t remaining = size;
  while (remaining > 0) {
    const size_t chunk = remaining < IO_BUFFER_SIZE ? remaining : IO_BUFFER_SIZE;
    const int readBytes = src.read(ioBuffer_.get(), chunk);
    if (readBytes <= 0 || static_cast<size_t>(readBytes) != chunk || !writeAll(ioBuffer_.get(), chunk)) {
      LOG_ERR("MDZ", "Failed copying '%s' into archive", srcPath.c_str());
      failed_ = true;
      return false;
    }
    remaining -= static_cast<uint32_t>(chunk);
  }

  entries_.push_back({std::string(zipEntryName), crc32, size, headerOffset});
  return true;
}

bool StoredZipWriter::finish() {
  if (failed_) {
    return false;
  }

  const auto centralDirOffset = static_cast<uint32_t>(out_.position());

  for (const auto& e : entries_) {
    bool ok = true;
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&CENTRAL_HEADER_SIG), sizeof(CENTRAL_HEADER_SIG));
    constexpr uint16_t versionMadeBy = VERSION_NEEDED;
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&versionMadeBy), sizeof(versionMadeBy));
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&VERSION_NEEDED), sizeof(VERSION_NEEDED));
    constexpr uint16_t generalPurposeFlag = 0;
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&generalPurposeFlag), sizeof(generalPurposeFlag));
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&METHOD_STORED), sizeof(METHOD_STORED));
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&DOS_TIME), sizeof(DOS_TIME));
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&DOS_DATE), sizeof(DOS_DATE));
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&e.crc), sizeof(e.crc));
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&e.size), sizeof(e.size));  // compressed size
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&e.size), sizeof(e.size));  // uncompressed size
    const auto nameLen = static_cast<uint16_t>(e.name.size());
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&nameLen), sizeof(nameLen));
    constexpr uint16_t extraLen = 0;
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&extraLen), sizeof(extraLen));
    constexpr uint16_t commentLen = 0;
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&commentLen), sizeof(commentLen));
    constexpr uint16_t diskNumStart = 0;
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&diskNumStart), sizeof(diskNumStart));
    constexpr uint16_t internalAttrs = 0;
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&internalAttrs), sizeof(internalAttrs));
    constexpr uint32_t externalAttrs = 0;
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&externalAttrs), sizeof(externalAttrs));
    ok &= writeAll(reinterpret_cast<const uint8_t*>(&e.localHeaderOffset), sizeof(e.localHeaderOffset));
    ok &= writeAll(reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size());

    if (!ok) {
      LOG_ERR("MDZ", "Failed writing central directory entry for '%s'", e.name.c_str());
      failed_ = true;
      return false;
    }
  }

  const auto centralDirSize = static_cast<uint32_t>(out_.position()) - centralDirOffset;
  const auto entryCount = static_cast<uint16_t>(entries_.size());

  bool ok = true;
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&EOCD_SIG), sizeof(EOCD_SIG));
  constexpr uint16_t diskNum = 0;
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&diskNum), sizeof(diskNum));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&diskNum), sizeof(diskNum));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&entryCount), sizeof(entryCount));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&entryCount), sizeof(entryCount));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&centralDirSize), sizeof(centralDirSize));
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&centralDirOffset), sizeof(centralDirOffset));
  constexpr uint16_t commentLen = 0;
  ok &= writeAll(reinterpret_cast<const uint8_t*>(&commentLen), sizeof(commentLen));

  if (!ok) {
    LOG_ERR("MDZ", "Failed writing EOCD record");
    failed_ = true;
    return false;
  }

  out_.flush();
  return true;
}
