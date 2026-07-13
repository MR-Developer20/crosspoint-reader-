#include "JwpubProbe.h"

#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <MiniSqlite.h>
#include <ZipFile.h>

#include <cstdlib>
#include <cstring>

#include "JwpubCrypto.h"
#include "JwpubManifest.h"

namespace {

// Extract a single member of `zip` to `outPath`. Closes the output before returning so the
// file can be reopened (e.g. as another ZipFile / database).
bool extractMember(ZipFile& zip, const char* member, const std::string& outPath) {
  HalFile out;
  if (!Storage.openFileForWrite("JWPROBE", outPath, out)) {
    LOG_ERR("JWPROBE", "cannot open %s for write", outPath.c_str());
    return false;
  }
  const bool ok = zip.readFileToStream(member, out, 4096);
  out.close();  // flush before any reopen of this path
  if (!ok) {
    LOG_ERR("JWPROBE", "failed to extract member '%s'", member);
    Storage.remove(outPath.c_str());
  }
  return ok;
}

// Find the first member whose name ends in ".db" (case-insensitive).
std::string findDbMember(ZipFile& zip) {
  std::string found;
  zip.enumerateFilePaths([&found](std::string_view name) {
    if (!found.empty() || name.size() < 3) {
      return;
    }
    const std::string_view tail = name.substr(name.size() - 3);
    if (tail[0] == '.' && (tail[1] == 'd' || tail[1] == 'D') && (tail[2] == 'b' || tail[2] == 'B')) {
      found = std::string(name);
    }
  });
  return found;
}

// Read the first Document.Content blob, decrypt it, and inflate a prefix to confirm HTML.
// Split out so the ZipFile/MiniSqlite referencing dbPath are gone before the caller deletes it.
bool runOnDatabase(const std::string& dbPath, const uint8_t keyIv[JwpubCrypto::KEY_IV_SIZE]) {
  MiniSqlite db;
  if (!db.open(dbPath)) {
    return false;
  }

  MiniSqlite::TableInfo doc;
  if (!db.findTable("Document", doc)) {
    LOG_ERR("JWPROBE", "no Document table");
    return false;
  }
  const int contentCol = doc.columnIndex("Content");
  const int titleCol = doc.columnIndex("Title");
  if (contentCol < 0) {
    LOG_ERR("JWPROBE", "Document has no Content column");
    return false;
  }

  MiniSqlite::Cursor cur = db.scan(doc.rootPage);
  if (!cur.next()) {
    LOG_ERR("JWPROBE", "Document table is empty (ok=%d)", cur.ok());
    return false;
  }

  if (titleCol >= 0) {
    std::string title;
    if (cur.getText(titleCol, title)) {
      LOG_INF("JWPROBE", "first document title: %s", title.c_str());
    }
  }

  size_t blobLen = 0;
  auto blob = cur.getColumnBytes(contentCol, &blobLen);
  if (!blob || blobLen == 0) {
    LOG_ERR("JWPROBE", "could not read Content blob");
    return false;
  }
  LOG_INF("JWPROBE", "Content blob: %zu bytes (rowid=%lld)", blobLen, static_cast<long long>(cur.rowid()));

  // AES-128-CBC decrypt in place.
  size_t clearLen = 0;
  if (!JwpubCrypto::decryptInPlace(keyIv, blob.get(), blobLen, &clearLen)) {
    LOG_ERR("JWPROBE", "decrypt failed (wrong key material?)");
    return false;
  }
  LOG_INF("JWPROBE", "decrypted %zu bytes; first bytes: %02x %02x %02x", clearLen, clearLen > 0 ? blob[0] : 0,
          clearLen > 1 ? blob[1] : 0, clearLen > 2 ? blob[2] : 0);
  if (clearLen < 2 || blob[0] != 0x78) {
    LOG_ERR("JWPROBE", "decrypted data is not a zlib stream (expected 0x78 header)");
    return false;
  }

  // zlib inflate the first chunk to confirm it is HTML.
  InflateReader inf;
  inf.init(false);
  inf.setSource(blob.get(), clearLen);
  inf.skipZlibHeader();
  uint8_t html[192];
  size_t produced = 0;
  const InflateStatus st = inf.readAtMost(html, sizeof(html) - 1, &produced);
  if (st == InflateStatus::Error || produced == 0) {
    LOG_ERR("JWPROBE", "inflate failed");
    return false;
  }
  html[produced] = '\0';
  LOG_INF("JWPROBE", "inflated %zu bytes; head: %s", produced, reinterpret_cast<const char*>(html));
  return true;
}

}  // namespace

bool JwpubProbe::run(const std::string& jwpubPath, const std::string& scratchDir) {
  LOG_INF("JWPROBE", "=== JWPUB probe: %s ===", jwpubPath.c_str());

  // 1. Outer ZIP -> manifest.json.
  ZipFile outer(jwpubPath);
  size_t manifestSize = 0;
  uint8_t* manifestRaw = outer.readFileToMemory("manifest.json", &manifestSize, /*trailingNullByte=*/true);
  if (!manifestRaw) {
    LOG_ERR("JWPROBE", "no manifest.json in outer zip");
    return false;
  }
  const ScopedCleanup freeManifest{[manifestRaw] { free(manifestRaw); }};  // malloc'd by ZipFile

  JwpubManifest manifest;
  if (!manifest.parse(reinterpret_cast<const char*>(manifestRaw))) {
    return false;
  }
  LOG_INF("JWPROBE", "manifest ok: symbol=%s year=%d issue=%d lang=%d fmt=%s", manifest.symbol.c_str(), manifest.year,
          manifest.issueTagNumber, manifest.mepsLanguageIndex, manifest.contentFormat.c_str());
  if (!manifest.hasKeyMaterial()) {
    LOG_ERR("JWPROBE", "manifest missing key-derivation fields");
    return false;
  }

  // 2. Derive the AES key/IV.
  uint8_t keyIv[JwpubCrypto::KEY_IV_SIZE];
  if (!JwpubCrypto::deriveKeyIv(manifest.mepsLanguageIndex, manifest.symbol, manifest.year, manifest.issueTagNumber,
                                keyIv)) {
    return false;
  }

  // 3. Inner `contents` ZIP -> scratch, then the `.db` inside it -> scratch.
  const std::string contentsPath = scratchDir + "/jwprobe_contents.zip";
  const std::string dbPath = scratchDir + "/jwprobe.db";
  Storage.mkdir(scratchDir.c_str());
  if (!extractMember(outer, "contents", contentsPath)) {
    return false;
  }

  bool result = false;
  {
    ZipFile inner(contentsPath);
    const std::string dbMember = findDbMember(inner);
    if (dbMember.empty()) {
      LOG_ERR("JWPROBE", "no .db member inside contents");
    } else {
      LOG_INF("JWPROBE", "database member: %s", dbMember.c_str());
      if (extractMember(inner, dbMember.c_str(), dbPath)) {
        result = runOnDatabase(dbPath, keyIv);
      }
    }
  }

  Storage.remove(contentsPath.c_str());
  Storage.remove(dbPath.c_str());
  LOG_INF("JWPROBE", "=== probe %s ===", result ? "PASSED" : "FAILED");
  return result;
}
