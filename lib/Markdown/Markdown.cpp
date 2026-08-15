#include "Markdown.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MinizConfig.h>
#include <Serialization.h>

#include <cstring>
#include <string_view>
#include <utility>

#include "MarkdownToXhtml.h"
#include "StoredZipWriter.h"

namespace {

constexpr uint32_t META_MAGIC = 0x4D444D31;  // "MDM1"
constexpr uint8_t META_VERSION = 1;

// Small write-through helper shared by the OPF/NCX generators below: streams to a HalFile
// while chaining a running CRC-32 and byte count, so StoredZipWriter::addFileEntryWithKnownCrc
// can copy the result into the archive without a redundant read-back pass. These documents
// are written to a scratch file rather than assembled as one in-RAM std::string because their
// size scales with heading/image count, which isn't bounded at the Markdown layer.
struct Writer {
  HalFile& out;
  uint32_t crc = MZ_CRC32_INIT;
  uint32_t size = 0;
  bool ok = true;

  void write(const char* data, size_t len) {
    if (!ok || len == 0) return;
    if (out.write(reinterpret_cast<const uint8_t*>(data), len) != len) {
      ok = false;
      return;
    }
    crc = mz_crc32(crc, reinterpret_cast<const uint8_t*>(data), len);
    size += static_cast<uint32_t>(len);
  }
  void write(std::string_view s) { write(s.data(), s.size()); }

  void writeEscaped(std::string_view s) {
    size_t runStart = 0;
    for (size_t i = 0; i < s.size(); i++) {
      const char* entity = nullptr;
      switch (s[i]) {
        case '&':
          entity = "&amp;";
          break;
        case '<':
          entity = "&lt;";
          break;
        case '>':
          entity = "&gt;";
          break;
        case '"':
          entity = "&quot;";
          break;
        default:
          continue;
      }
      if (i > runStart) write(s.data() + runStart, i - runStart);
      write(entity, strlen(entity));
      runStart = i + 1;
    }
    if (runStart < s.size()) write(s.data() + runStart, s.size() - runStart);
  }
};

constexpr std::string_view STYLE_CSS =
    "h1 { margin-top: 0; margin-bottom: 1em; }\n"
    "h2 { margin-top: 1.5em; margin-bottom: 0.8em; }\n"
    "h3 { margin-top: 1.2em; margin-bottom: 0.6em; }\n"
    "h4, h5, h6 { margin-top: 1em; margin-bottom: 0.5em; text-align: left; }\n"
    "blockquote.q1 { margin-left: 1.5em; margin-right: 1em; font-style: italic; }\n"
    "blockquote.q2 { margin-left: 3em; margin-right: 1em; font-style: italic; }\n"
    "blockquote.q3 { margin-left: 4.5em; margin-right: 1em; font-style: italic; }\n"
    "blockquote.q4 { margin-left: 6em; margin-right: 1em; font-style: italic; }\n"
    "li.ul1 { margin-left: 1.5em; text-align: left; text-indent: 0; }\n"
    "li.ul2 { margin-left: 3em; text-align: left; text-indent: 0; }\n"
    "li.ul3 { margin-left: 4.5em; text-align: left; text-indent: 0; }\n"
    "li.ul4 { margin-left: 6em; text-align: left; text-indent: 0; }\n"
    "div.ol1 { margin-left: 1.5em; text-align: left; text-indent: 0; }\n"
    "div.ol2 { margin-left: 3em; text-align: left; text-indent: 0; }\n"
    "div.ol3 { margin-left: 4.5em; text-align: left; text-indent: 0; }\n"
    "div.ol4 { margin-left: 6em; text-align: left; text-indent: 0; }\n"
    "p.code { margin-left: 1.5em; text-align: left; text-indent: 0; }\n";

constexpr std::string_view MIMETYPE = "application/epub+zip";

// Derives a book title from the source filename: strips the folder and the .md extension.
std::string titleFromFilename(const std::string& mdPath) {
  const size_t lastSlash = mdPath.find_last_of('/');
  std::string name = lastSlash == std::string::npos ? mdPath : mdPath.substr(lastSlash + 1);
  if (FsHelpers::hasMarkdownExtension(name)) {
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
  }
  return name;
}

bool writeContainerXml(StoredZipWriter& zip) {
  static constexpr std::string_view kXml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
      "  <rootfiles>\n"
      "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
      "  </rootfiles>\n"
      "</container>\n";
  return zip.addBufferEntry("META-INF/container.xml", reinterpret_cast<const uint8_t*>(kXml.data()), kXml.size());
}

bool writeContentOpf(const std::string& tmpPath, const std::string& title,
                     const std::vector<MarkdownToXhtml::ImageRef>& images, uint32_t* crc, uint32_t* size) {
  HalFile out;
  if (!Storage.openFileForWrite("MD", tmpPath, out)) {
    LOG_ERR("MD", "Cannot open scratch file: %s", tmpPath.c_str());
    return false;
  }
  Writer w{out};

  w.write(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<package version=\"2.0\" xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\">\n"
      "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n"
      "    <dc:title>");
  w.writeEscaped(title);
  w.write(
      "</dc:title>\n"
      "    <dc:language>en</dc:language>\n"
      "  </metadata>\n"
      "  <manifest>\n"
      "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
      "    <item id=\"index\" href=\"index.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "    <item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n");

  int imgIndex = 0;
  for (const auto& img : images) {
    const bool isPng =
        img.zipEntryName.size() >= 4 && img.zipEntryName.compare(img.zipEntryName.size() - 4, 4, ".png") == 0;
    w.write("    <item id=\"img");
    w.write(std::to_string(imgIndex++));
    w.write("\" href=\"images/");
    // zipEntryName is "OEBPS/images/imgN.ext" — the manifest href is relative to OEBPS/.
    w.write(std::string_view(img.zipEntryName).substr(sizeof("OEBPS/images/") - 1));
    w.write("\" media-type=\"image/");
    w.write(isPng ? "png" : "jpeg");
    w.write("\"/>\n");
  }

  w.write(
      "  </manifest>\n"
      "  <spine toc=\"ncx\">\n"
      "    <itemref idref=\"index\"/>\n"
      "  </spine>\n"
      "</package>\n");

  if (!w.ok) {
    LOG_ERR("MD", "Failed writing content.opf");
    return false;
  }
  *crc = w.crc;
  *size = w.size;
  return true;
}

// One flat navMap — every heading is a sibling navPoint regardless of level, so the
// generator needs no stack to track nested open/close tags. This loses the heading-level
// hierarchy in the chapter list's indentation, but every heading is still independently
// navigable, which is what "Select Chapter" needs.
bool writeTocNcx(const std::string& tmpPath, const std::string& title,
                 const std::vector<MarkdownToXhtml::Heading>& headings, uint32_t* crc, uint32_t* size) {
  HalFile out;
  if (!Storage.openFileForWrite("MD", tmpPath, out)) {
    LOG_ERR("MD", "Cannot open scratch file: %s", tmpPath.c_str());
    return false;
  }
  Writer w{out};

  w.write(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
      "  <head>\n"
      "    <meta name=\"dtb:uid\" content=\"crosspoint-md\"/>\n"
      "  </head>\n"
      "  <docTitle><text>");
  w.writeEscaped(title);
  w.write("</text></docTitle>\n  <navMap>\n");

  int order = 1;
  for (const auto& h : headings) {
    w.write("    <navPoint id=\"np");
    w.write(std::to_string(order - 1));
    w.write("\" playOrder=\"");
    w.write(std::to_string(order));
    w.write("\">\n      <navLabel><text>");
    w.writeEscaped(h.text.empty() ? std::string_view("(untitled)") : std::string_view(h.text));
    w.write("</text></navLabel>\n      <content src=\"index.xhtml#");
    w.write(h.anchorId);
    w.write("\"/>\n    </navPoint>\n");
    order++;
  }

  w.write("  </navMap>\n</ncx>\n");

  if (!w.ok) {
    LOG_ERR("MD", "Failed writing toc.ncx");
    return false;
  }
  *crc = w.crc;
  *size = w.size;
  return true;
}

}  // namespace

Markdown::Markdown(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  cachePath = this->cacheBasePath + "/md_" + std::to_string(std::hash<std::string>{}(filepath));
  epubPath = cachePath + "/book.epub";
}

void Markdown::setupCacheDir() const {
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }
}

bool Markdown::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    return true;
  }
  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("MD", "Failed to clear cache: %s", cachePath.c_str());
    return false;
  }
  return true;
}

bool Markdown::isCacheFresh() const {
  const std::string metaPath = cachePath + "/source.meta";
  if (!Storage.exists(epubPath.c_str()) || !Storage.exists(metaPath.c_str())) {
    return false;
  }

  HalFile meta;
  if (!Storage.openFileForRead("MD", metaPath, meta)) {
    return false;
  }
  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t storedSize = 0;
  serialization::readPod(meta, magic);
  serialization::readPod(meta, version);
  serialization::readPod(meta, storedSize);
  if (magic != META_MAGIC || version != META_VERSION) {
    return false;
  }

  HalFile src;
  if (!Storage.openFileForRead("MD", filepath, src)) {
    return false;
  }
  return static_cast<uint32_t>(src.size()) == storedSize;
}

bool Markdown::convertToEpub() const {
  setupCacheDir();

  const std::string xhtmlTmpPath = cachePath + "/index.xhtml.tmp";
  const std::string opfTmpPath = cachePath + "/content.opf.tmp";
  const std::string ncxTmpPath = cachePath + "/toc.ncx.tmp";
  const std::string epubTmpPath = cachePath + "/book.epub.tmp";

  const MarkdownToXhtml::Result conv = MarkdownToXhtml::convert(filepath, xhtmlTmpPath);
  if (!conv.ok) {
    LOG_ERR("MD", "Markdown conversion failed: %s", filepath.c_str());
    Storage.remove(xhtmlTmpPath.c_str());
    return false;
  }

  const std::string title = conv.firstH1.empty() ? titleFromFilename(filepath) : conv.firstH1;

  uint32_t opfCrc = 0, opfSize = 0;
  uint32_t ncxCrc = 0, ncxSize = 0;
  bool ok = writeContentOpf(opfTmpPath, title, conv.images, &opfCrc, &opfSize) &&
            writeTocNcx(ncxTmpPath, title, conv.headings, &ncxCrc, &ncxSize);

  if (ok) {
    // expectedEntries: mimetype + container + opf + ncx + css + xhtml + one per image.
    HalFile epubOut;
    if (!Storage.openFileForWrite("MD", epubTmpPath, epubOut)) {
      LOG_ERR("MD", "Cannot open scratch epub: %s", epubTmpPath.c_str());
      ok = false;
    } else {
      StoredZipWriter zip(epubOut, 6 + conv.images.size());
      ok =
          zip.addBufferEntry("mimetype", reinterpret_cast<const uint8_t*>(MIMETYPE.data()), MIMETYPE.size()) &&
          writeContainerXml(zip) && zip.addFileEntryWithKnownCrc("OEBPS/content.opf", opfTmpPath, opfCrc, opfSize) &&
          zip.addFileEntryWithKnownCrc("OEBPS/toc.ncx", ncxTmpPath, ncxCrc, ncxSize) &&
          zip.addBufferEntry("OEBPS/style.css", reinterpret_cast<const uint8_t*>(STYLE_CSS.data()), STYLE_CSS.size()) &&
          zip.addFileEntryWithKnownCrc("OEBPS/index.xhtml", xhtmlTmpPath, conv.crc32, conv.size);
      for (const auto& img : conv.images) {
        ok = ok && zip.addFileEntry(img.zipEntryName, img.sdPath);
      }
      ok = ok && zip.finish();
      // epubOut closes here (scope exit) before the rename below.
    }
  }

  Storage.remove(xhtmlTmpPath.c_str());
  Storage.remove(opfTmpPath.c_str());
  Storage.remove(ncxTmpPath.c_str());

  if (!ok) {
    LOG_ERR("MD", "Failed to package synthetic EPUB for %s", filepath.c_str());
    Storage.remove(epubTmpPath.c_str());
    return false;
  }

  // book.epub keeps a fixed path across regenerations (so the .md's own cache dir doesn't
  // grow unbounded), but the wrapping Epub's cache key is a hash of that same path — so its
  // book.bin/sections/* cache would otherwise survive untouched even though the content at
  // that path just changed underneath it, since Epub's own cache validation only checks a
  // format-version byte, never the source zip's content (true for any EPUB, not just this
  // generated one). Explicitly purge it here, every regeneration, before the new book.epub
  // is even in place, so a reader opening it next always rebuilds against the new content.
  Epub(epubPath, cacheBasePath).clearCache();

  // Crash-safe swap: never leave book.epub half-written (see ProgressFile::writeAtomic for
  // the same remove-then-rename pattern and why SdFat needs it — rename doesn't overwrite).
  Storage.remove(epubPath.c_str());
  if (!Storage.rename(epubTmpPath.c_str(), epubPath.c_str())) {
    LOG_ERR("MD", "Failed to move scratch epub into place: %s", epubPath.c_str());
    return false;
  }

  const std::string metaTmpPath = cachePath + "/source.meta.tmp";
  const std::string metaPath = cachePath + "/source.meta";
  HalFile src;
  uint32_t sourceSize = 0;
  if (Storage.openFileForRead("MD", filepath, src)) {
    sourceSize = static_cast<uint32_t>(src.size());
  }
  {
    HalFile metaOut;
    if (Storage.openFileForWrite("MD", metaTmpPath, metaOut)) {
      serialization::writePod(metaOut, META_MAGIC);
      serialization::writePod(metaOut, META_VERSION);
      serialization::writePod(metaOut, sourceSize);
    }
  }
  Storage.remove(metaPath.c_str());
  Storage.rename(metaTmpPath.c_str(), metaPath.c_str());

  return true;
}

bool Markdown::load() {
  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR("MD", "File does not exist: %s", filepath.c_str());
    return false;
  }

  if (isCacheFresh()) {
    loaded = true;
    return true;
  }

  if (!convertToEpub()) {
    return false;
  }

  loaded = true;
  return true;
}
