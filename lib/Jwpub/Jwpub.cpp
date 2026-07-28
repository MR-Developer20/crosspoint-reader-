#include "Jwpub.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>
#include <MiniSqlite.h>
#include <ZipFile.h>

#include <cstdlib>
#include <cstring>
#include <string_view>

#include "JwpubCrypto.h"
#include "JwpubManifest.h"

namespace {

// Documents whose decrypted+inflated HTML is far larger than any real article are skipped
// as a safety valve; nothing in a normal publication approaches this.
constexpr size_t MAX_DOC_INFLATED = 4 * 1024 * 1024;

// Streaming HTML → plain-text stripper. Fed inflated HTML in arbitrary chunks (tags and
// entities may straddle chunk boundaries), it appends readable text to `out`: tags are
// dropped, block-level boundaries become newlines, runs of whitespace collapse to a single
// space, and the common/numeric character entities are decoded to UTF-8.
class HtmlTextStripper {
 public:
  explicit HtmlTextStripper(std::string& out) : out_(out) {}

  void feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
      process(static_cast<char>(data[i]));
    }
  }

  // Flush a dangling, unterminated entity (rare, malformed input) as literal text.
  void finish() {
    if (inEntity_) {
      putChar('&');
      for (const char e : entity_) {
        putChar(e);
      }
      inEntity_ = false;
      entity_.clear();
    }
  }

 private:
  static bool isTagNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  }
  static bool isEntityChar(char c) { return isTagNameChar(c) || c == '#'; }

  static bool isBlockTag(const std::string& t) {
    static const char* kBlock[] = {"p",          "div",   "br", "li",         "tr",      "td",      "th",
                                   "h1",         "h2",    "h3", "h4",         "h5",      "h6",      "ul",
                                   "ol",         "table", "hr", "blockquote", "section", "article", "figure",
                                   "figcaption", "dd",    "dt", "caption",    "header",  "footer"};
    for (const char* b : kBlock) {
      if (t == b) {
        return true;
      }
    }
    return false;
  }

  void process(char c) {
    if (inTag_) {
      inTagChar(c);
      return;
    }
    if (inEntity_) {
      inEntityChar(c);
      return;
    }
    if (c == '<') {
      inTag_ = true;
      tagNameDone_ = false;
      tagName_.clear();
      return;
    }
    if (c == '&') {
      inEntity_ = true;
      entity_.clear();
      return;
    }
    putChar(c);
  }

  void inTagChar(char c) {
    if (c == '>') {
      inTag_ = false;
      if (isBlockTag(tagName_)) {
        putNewline();
      }
      return;
    }
    if (!tagNameDone_) {
      if (c == '/' && tagName_.empty()) {
        return;  // closing-tag slash, keep collecting the name
      }
      if (isTagNameChar(c)) {
        tagName_.push_back(static_cast<char>(c | 0x20));  // lowercase
      } else {
        tagNameDone_ = true;  // name ended (attributes / whitespace follow)
      }
    }
  }

  void inEntityChar(char c) {
    if (c == ';') {
      emitEntity(entity_);
      inEntity_ = false;
      entity_.clear();
      return;
    }
    if (entity_.size() < 12 && isEntityChar(c)) {
      entity_.push_back(c);
      return;
    }
    // Malformed entity: emit what we have literally, then reprocess the current char.
    putChar('&');
    for (const char e : entity_) {
      putChar(e);
    }
    inEntity_ = false;
    entity_.clear();
    process(c);
  }

  void emitEntity(const std::string& name) {
    if (name.empty()) {
      return;
    }
    if (name[0] == '#') {  // numeric character reference
      uint32_t cp = 0;
      if (name.size() > 1 && (name[1] == 'x' || name[1] == 'X')) {
        for (size_t i = 2; i < name.size(); i++) {
          const char d = name[i];
          cp <<= 4;
          if (d >= '0' && d <= '9') {
            cp |= static_cast<uint32_t>(d - '0');
          } else if (d >= 'a' && d <= 'f') {
            cp |= static_cast<uint32_t>(10 + d - 'a');
          } else if (d >= 'A' && d <= 'F') {
            cp |= static_cast<uint32_t>(10 + d - 'A');
          } else {
            return;
          }
        }
      } else {
        for (size_t i = 1; i < name.size(); i++) {
          if (name[i] < '0' || name[i] > '9') {
            return;
          }
          cp = cp * 10 + static_cast<uint32_t>(name[i] - '0');
        }
      }
      putCodepoint(cp);
      return;
    }
    if (name == "amp") {
      putChar('&');
    } else if (name == "lt") {
      putChar('<');
    } else if (name == "gt") {
      putChar('>');
    } else if (name == "quot") {
      putChar('"');
    } else if (name == "apos") {
      putChar('\'');
    } else if (name == "nbsp") {
      putChar(' ');
    } else if (name == "mdash") {
      putCodepoint(0x2014);
    } else if (name == "ndash") {
      putCodepoint(0x2013);
    } else if (name == "hellip") {
      putCodepoint(0x2026);
    } else if (name == "rsquo") {
      putCodepoint(0x2019);
    } else if (name == "lsquo") {
      putCodepoint(0x2018);
    } else if (name == "rdquo") {
      putCodepoint(0x201D);
    } else if (name == "ldquo") {
      putCodepoint(0x201C);
    }
    // Unknown named entity: drop it (rare; avoids leaking raw &name; into the text).
  }

  void putChar(char c) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!atLineStart_) {
        pendingSpace_ = true;
      }
      return;
    }
    if (pendingSpace_) {
      out_.push_back(' ');
      pendingSpace_ = false;
    }
    out_.push_back(c);
    atLineStart_ = false;
  }

  void putNewline() {
    pendingSpace_ = false;
    if (!atLineStart_) {
      out_.push_back('\n');
      atLineStart_ = true;
    }
  }

  void putRawByte(uint8_t b) {
    if (pendingSpace_) {
      out_.push_back(' ');
      pendingSpace_ = false;
    }
    out_.push_back(static_cast<char>(b));
    atLineStart_ = false;
  }

  void putCodepoint(uint32_t cp) {
    if (cp == 0) {
      return;
    }
    if (cp < 0x80) {
      putChar(static_cast<char>(cp));
    } else if (cp < 0x800) {
      putRawByte(static_cast<uint8_t>(0xC0 | (cp >> 6)));
      putRawByte(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      putRawByte(static_cast<uint8_t>(0xE0 | (cp >> 12)));
      putRawByte(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
      putRawByte(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
      putRawByte(static_cast<uint8_t>(0xF0 | (cp >> 18)));
      putRawByte(static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F)));
      putRawByte(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
      putRawByte(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
    }
  }

  std::string& out_;
  bool inTag_ = false;
  bool tagNameDone_ = false;
  std::string tagName_;
  bool inEntity_ = false;
  std::string entity_;
  bool pendingSpace_ = false;
  bool atLineStart_ = true;
};

// Flush `buf` to `out` if it has grown past the threshold. Returns false on write failure.
bool flushIfLarge(HalFile& out, std::string& buf, size_t threshold) {
  if (buf.size() < threshold) {
    return true;
  }
  const bool ok = out.write(reinterpret_cast<const uint8_t*>(buf.data()), buf.size()) == buf.size();
  buf.clear();
  return ok;
}

// Find the first member of `zip` whose name ends in ".db" (case-insensitive).
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

// Read every Document, decrypt/inflate/strip its Content, and write the concatenated text
// to `outPath`. Returns true if at least one document was written.
bool writeContentText(const std::string& dbPath, const uint8_t keyIv[JwpubCrypto::KEY_IV_SIZE],
                      const std::string& outPath) {
  MiniSqlite db;
  if (!db.open(dbPath)) {
    return false;
  }
  MiniSqlite::TableInfo doc;
  if (!db.findTable("Document", doc)) {
    LOG_ERR("JWPUB", "no Document table in database");
    return false;
  }
  const int contentCol = doc.columnIndex("Content");
  const int titleCol = doc.columnIndex("Title");
  if (contentCol < 0) {
    LOG_ERR("JWPUB", "Document table has no Content column");
    return false;
  }

  HalFile out;
  if (!Storage.openFileForWrite("JWPUB", outPath, out)) {
    return false;
  }

  InflateStream inflate;  // one instance, reused across documents (allocations are retained)
  std::string buf;
  buf.reserve(8192);
  size_t docCount = 0;

  MiniSqlite::Cursor cur = db.scan(doc.rootPage);
  while (cur.next()) {
    size_t encLen = 0;
    auto enc = cur.getColumnBytes(contentCol, &encLen);
    if (!enc || encLen == 0) {
      continue;
    }
    size_t zlibLen = 0;
    if (!JwpubCrypto::decryptInPlace(keyIv, enc.get(), encLen, &zlibLen)) {
      LOG_ERR("JWPUB", "decrypt failed for document rowid=%lld", static_cast<long long>(cur.rowid()));
      continue;
    }

    // Optional plaintext document title as a heading line.
    if (titleCol >= 0) {
      std::string docTitle;
      if (cur.getText(titleCol, docTitle) && !docTitle.empty()) {
        buf += docTitle;
        buf.push_back('\n');
      }
    }

    if (!inflate.init(/*streaming=*/true)) {
      LOG_ERR("JWPUB", "inflate init OOM");
      return false;
    }
    inflate.setSource(enc.get(), zlibLen);
    inflate.setZlibWrapped();

    HtmlTextStripper stripper(buf);
    uint8_t chunk[1024];
    bool inflateOk = true;
    size_t docInflated = 0;
    while (true) {
      size_t produced = 0;
      const InflateStream::Status status = inflate.readAtMost(chunk, sizeof(chunk), &produced);
      if (produced > 0) {
        stripper.feed(chunk, produced);
        docInflated += produced;
        if (docInflated > MAX_DOC_INFLATED) {  // runaway / zlib-bomb guard
          inflateOk = false;
          break;
        }
      }
      if (status == InflateStream::Status::Done) {
        break;
      }
      if (status == InflateStream::Status::Error || (status == InflateStream::Status::Ok && produced == 0)) {
        inflateOk = false;
        break;
      }
      if (!flushIfLarge(out, buf, 4096)) {
        return false;
      }
    }
    stripper.finish();
    if (!inflateOk) {
      LOG_ERR("JWPUB", "inflate failed for document rowid=%lld", static_cast<long long>(cur.rowid()));
      buf.clear();
      continue;
    }

    buf += "\n\n";  // blank line between documents
    if (out.write(reinterpret_cast<const uint8_t*>(buf.data()), buf.size()) != buf.size()) {
      return false;
    }
    buf.clear();
    docCount++;
  }

  LOG_INF("JWPUB", "converted %zu documents", docCount);
  return docCount > 0;
}

}  // namespace

Jwpub::Jwpub(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  cachePath = this->cacheBasePath + "/jwpub_" + std::to_string(std::hash<std::string>{}(filepath));
}

void Jwpub::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) {
    Storage.mkdir(cacheBasePath.c_str());
  }
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }
}

bool Jwpub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    return true;
  }
  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("JWPUB", "failed to clear cache: %s", cachePath.c_str());
    return false;
  }
  return true;
}

bool Jwpub::convert() {
  // 1. Outer ZIP → manifest.json → key/IV.
  ZipFile outer(filepath);
  size_t manifestSize = 0;
  uint8_t* manifestRaw = outer.readFileToMemory("manifest.json", &manifestSize, /*trailingNullByte=*/true);
  if (!manifestRaw) {
    LOG_ERR("JWPUB", "no manifest.json in %s", filepath.c_str());
    return false;
  }
  const ScopedCleanup freeManifest{[manifestRaw] { free(manifestRaw); }};  // malloc'd by ZipFile

  JwpubManifest manifest;
  if (!manifest.parse(reinterpret_cast<const char*>(manifestRaw)) || !manifest.hasKeyMaterial()) {
    LOG_ERR("JWPUB", "manifest missing key-derivation fields");
    return false;
  }
  if (!manifest.title.empty()) {
    title = manifest.title;
  }
  uint8_t keyIv[JwpubCrypto::KEY_IV_SIZE];
  if (!JwpubCrypto::deriveKeyIv(manifest.mepsLanguageIndex, manifest.symbol, manifest.year, manifest.issueTagNumber,
                                keyIv)) {
    return false;
  }

  // 2. Stage the inner `contents` ZIP, then extract the `.db` from it.
  setupCacheDir();
  const std::string contentsPath = cachePath + "/contents.zip";
  const std::string dbPath = cachePath + "/pub.db";

  {
    HalFile out;
    if (!Storage.openFileForWrite("JWPUB", contentsPath, out)) {
      return false;
    }
    if (!outer.readFileToStream("contents", out, 4096)) {
      out.close();
      Storage.remove(contentsPath.c_str());
      LOG_ERR("JWPUB", "failed to extract inner contents");
      return false;
    }
  }

  bool extractedDb = false;
  {
    ZipFile inner(contentsPath);
    const std::string dbMember = findDbMember(inner);
    if (dbMember.empty()) {
      LOG_ERR("JWPUB", "no .db inside contents");
    } else {
      HalFile out;
      if (Storage.openFileForWrite("JWPUB", dbPath, out)) {
        extractedDb = inner.readFileToStream(dbMember.c_str(), out, 4096);
      }
    }
  }
  Storage.remove(contentsPath.c_str());  // text-only MVP does not need the media/images
  if (!extractedDb) {
    LOG_ERR("JWPUB", "failed to extract database");
    Storage.remove(dbPath.c_str());
    return false;
  }

  // 3. Decrypt/inflate/strip every Document into content.txt (atomic via .part).
  const std::string partPath = getContentPath() + ".part";
  const bool ok = writeContentText(dbPath, keyIv, partPath);
  Storage.remove(dbPath.c_str());
  if (!ok) {
    Storage.remove(partPath.c_str());
    return false;
  }
  Storage.remove(getContentPath().c_str());
  if (!Storage.rename(partPath.c_str(), getContentPath().c_str())) {
    LOG_ERR("JWPUB", "failed to promote content.txt");
    Storage.remove(partPath.c_str());
    return false;
  }
  return true;
}

bool Jwpub::load() {
  if (loaded) {
    return true;
  }
  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR("JWPUB", "file does not exist: %s", filepath.c_str());
    return false;
  }

  // Fallback/fast-path title: the file name without folder or extension.
  {
    const size_t slash = filepath.find_last_of('/');
    std::string name = (slash == std::string::npos) ? filepath : filepath.substr(slash + 1);
    if (FsHelpers::hasJwpubExtension(name)) {
      name.resize(name.length() - 6);  // strip ".jwpub"
    }
    title = name;
  }

  if (Storage.exists(getContentPath().c_str())) {
    loaded = true;
    LOG_DBG("JWPUB", "reusing cached content.txt for %s", filepath.c_str());
    return true;
  }

  if (!convert()) {
    LOG_ERR("JWPUB", "conversion failed for %s", filepath.c_str());
    return false;
  }
  loaded = true;
  return true;
}
