#include "MarkdownToXhtml.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <MinizConfig.h>

#include <cctype>
#include <cstring>
#include <string_view>
#include <utility>

namespace {

constexpr size_t READ_CHUNK_SIZE = 4096;
constexpr size_t MAX_LINE_LEN = 8192;                // pathologically long lines are truncated, not grown without bound
constexpr int MAX_NEST_DEPTH = 4;                    // blockquote and list nesting cap (plan: "clamp deeper nesting")
constexpr size_t MAX_IMAGE_BYTES = 2 * 1024 * 1024;  // skip (fall back to alt text) above this

// Accumulates output through a HalFile while chaining a running CRC-32 and byte count, so
// the caller has both without a second read pass over the generated file.
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

  // Writes `s` with XML's three mandatory text-node characters escaped. No allocation:
  // flushes runs of "safe" bytes directly from the source buffer.
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

// Buffered line reader over a HalFile. Strips the trailing newline (and a preceding \r);
// reports whether the raw line ended with a CommonMark hard-break marker (two or more
// trailing spaces, or a trailing backslash) so the caller can insert a <br/> before the
// next line if the block continues. Lines longer than MAX_LINE_LEN are truncated (rest of
// the physical line is discarded, not buffered) to keep memory bounded.
class LineSource {
 public:
  explicit LineSource(HalFile& file) : file_(file) { buf_ = makeUniqueNoThrow<uint8_t[]>(READ_CHUNK_SIZE); }

  bool ok() const { return static_cast<bool>(buf_); }

  bool next(std::string& out, bool* endedWithHardBreak) {
    out.clear();
    bool any = false;
    bool discarding = false;

    while (true) {
      if (bufPos_ >= bufLen_) {
        if (eof_) break;
        const int n = file_.read(buf_.get(), READ_CHUNK_SIZE);
        if (n <= 0) {
          eof_ = true;
          break;
        }
        bufLen_ = static_cast<size_t>(n);
        bufPos_ = 0;
      }

      size_t i = bufPos_;
      while (i < bufLen_ && buf_[i] != '\n') i++;
      const size_t chunkLen = i - bufPos_;
      if (chunkLen > 0) {
        any = true;
        if (!discarding) {
          if (out.size() + chunkLen > MAX_LINE_LEN) {
            const size_t take = MAX_LINE_LEN > out.size() ? MAX_LINE_LEN - out.size() : 0;
            out.append(reinterpret_cast<const char*>(buf_.get() + bufPos_), take);
            discarding = true;
          } else {
            out.append(reinterpret_cast<const char*>(buf_.get() + bufPos_), chunkLen);
          }
        }
      }
      bufPos_ = i;
      if (bufPos_ < bufLen_) {
        bufPos_++;  // consume the newline
        any = true;
        break;
      }
      // buffer exhausted without finding '\n' — refill and keep scanning the same line
    }

    if (!any) return false;

    if (!out.empty() && out.back() == '\r') out.pop_back();

    bool hardBreak = false;
    size_t trailingSpaces = 0;
    while (trailingSpaces < out.size() && out[out.size() - 1 - trailingSpaces] == ' ') trailingSpaces++;
    if (trailingSpaces >= 2) {
      hardBreak = true;
    } else if (!out.empty() && out.back() == '\\') {
      hardBreak = true;
    }
    if (endedWithHardBreak) *endedWithHardBreak = hardBreak;
    return true;
  }

 private:
  HalFile& file_;
  std::unique_ptr<uint8_t[]> buf_;
  size_t bufLen_ = 0;
  size_t bufPos_ = 0;
  bool eof_ = false;
};

// ASCII-only uppercase for h1 differentiation (plan: headings can't be made larger, so h1
// is visually set apart by case instead). Bytes >= 0x80 (UTF-8 continuation/lead bytes) are
// left untouched so multi-byte characters are never corrupted.
std::string asciiUppercase(std::string_view s) {
  std::string result;
  result.reserve(s.size());
  for (const char c : s) {
    result.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
  }
  return result;
}

bool isAsciiPunct(char c) {
  return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

// Renders inline markdown (bold/italic/code spans/links/autolinks/escapes) from a single
// source line into XHTML. Constructs that need to span multiple lines are out of scope —
// see the class-level doc comment.
void renderInline(std::string_view line, Writer& w) {
  size_t i = 0;
  size_t runStart = 0;
  auto flush = [&](size_t end) {
    if (end > runStart) w.writeEscaped(line.substr(runStart, end - runStart));
  };

  while (i < line.size()) {
    const char c = line[i];

    if (c == '\\' && i + 1 < line.size() && isAsciiPunct(line[i + 1])) {
      flush(i);
      w.writeEscaped(line.substr(i + 1, 1));
      i += 2;
      runStart = i;
      continue;
    }

    if (c == '`') {
      const size_t close = line.find('`', i + 1);
      if (close != std::string_view::npos) {
        flush(i);
        w.writeEscaped(line.substr(i + 1, close - i - 1));
        i = close + 1;
        runStart = i;
        continue;
      }
    }

    if (c == '*' || c == '_') {
      const bool dbl = (i + 1 < line.size() && line[i + 1] == c);
      const size_t delimLen = dbl ? 2 : 1;
      const size_t contentStart = i + delimLen;
      // Left-flanking: no space immediately inside the opening delimiter.
      if (contentStart < line.size() && line[contentStart] != ' ' && line[contentStart] != '\t') {
        // Search for a matching closing run of the same length, not immediately preceded by
        // whitespace (right-flanking).
        size_t searchFrom = contentStart;
        size_t close = std::string_view::npos;
        while (searchFrom < line.size()) {
          const size_t cand = line.find(c, searchFrom);
          if (cand == std::string_view::npos) break;
          const bool candDbl = dbl && cand + 1 < line.size() && line[cand + 1] == c;
          if (dbl == candDbl && cand > contentStart && line[cand - 1] != ' ' && line[cand - 1] != '\t') {
            close = cand;
            break;
          }
          searchFrom = cand + 1;
        }
        if (close != std::string_view::npos) {
          flush(i);
          const char* tag = dbl ? "strong" : "em";
          w.write("<");
          w.write(tag);
          w.write(">");
          // Inner content is escaped plain text (no nested inline constructs — see class doc).
          w.writeEscaped(line.substr(contentStart, close - contentStart));
          w.write("</");
          w.write(tag);
          w.write(">");
          i = close + delimLen;
          runStart = i;
          continue;
        }
      }
    }

    if (c == '[') {
      const size_t textEnd = line.find(']', i + 1);
      if (textEnd != std::string_view::npos && textEnd + 1 < line.size() && line[textEnd + 1] == '(') {
        const size_t urlEnd = line.find(')', textEnd + 2);
        if (urlEnd != std::string_view::npos) {
          flush(i);
          // Link target is dropped (no browser to follow it) — only the visible text remains.
          w.writeEscaped(line.substr(i + 1, textEnd - i - 1));
          i = urlEnd + 1;
          runStart = i;
          continue;
        }
      }
    }

    if (c == '<') {
      const size_t close = line.find('>', i + 1);
      if (close != std::string_view::npos) {
        const std::string_view inner = line.substr(i + 1, close - i - 1);
        const bool looksLikeAutolink =
            !inner.empty() && inner.find(' ') == std::string_view::npos &&
            (inner.find("://") != std::string_view::npos || inner.find('@') != std::string_view::npos);
        if (looksLikeAutolink) {
          flush(i);
          w.writeEscaped(inner);
          i = close + 1;
          runStart = i;
          continue;
        }
      }
    }

    i++;
  }
  flush(line.size());
}

bool isBlank(std::string_view line) {
  for (char c : line) {
    if (c != ' ' && c != '\t') return false;
  }
  return true;
}

bool isThematicBreak(std::string_view line) {
  size_t i = 0;
  while (i < line.size() && i < 3 && line[i] == ' ') i++;
  char marker = 0;
  int count = 0;
  for (; i < line.size(); i++) {
    const char c = line[i];
    if (c == ' ' || c == '\t') continue;
    if (c == '-' || c == '_' || c == '*') {
      if (marker == 0) {
        marker = c;
      } else if (c != marker) {
        return false;
      }
      count++;
    } else {
      return false;
    }
  }
  return count >= 3;
}

bool parseAtxHeading(std::string_view line, int* level, std::string_view* text) {
  size_t i = 0;
  while (i < line.size() && i < 3 && line[i] == ' ') i++;
  size_t h = i;
  while (h < line.size() && line[h] == '#' && (h - i) < 6) h++;
  const int count = static_cast<int>(h - i);
  if (count < 1) return false;
  if (h < line.size() && line[h] != ' ' && line[h] != '\t') return false;

  size_t start = h;
  while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) start++;
  size_t end = line.size();
  while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) end--;

  size_t hashEnd = end;
  while (hashEnd > start && line[hashEnd - 1] == '#') hashEnd--;
  if (hashEnd < end && (hashEnd == start || line[hashEnd - 1] == ' ' || line[hashEnd - 1] == '\t')) {
    end = hashEnd;
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
  }

  *level = count;
  *text = line.substr(start, end - start);
  return true;
}

bool parseFenceStart(std::string_view line, char* fenceChar, int* fenceLen) {
  size_t i = 0;
  while (i < line.size() && i < 3 && line[i] == ' ') i++;
  if (i >= line.size()) return false;
  const char c = line[i];
  if (c != '`' && c != '~') return false;
  size_t j = i;
  while (j < line.size() && line[j] == c) j++;
  const int count = static_cast<int>(j - i);
  if (count < 3) return false;
  *fenceChar = c;
  *fenceLen = count;
  return true;
}

bool isFenceClose(std::string_view line, char fenceChar, int fenceLen) {
  size_t i = 0;
  while (i < line.size() && i < 3 && line[i] == ' ') i++;
  size_t j = i;
  while (j < line.size() && line[j] == fenceChar) j++;
  if (static_cast<int>(j - i) < fenceLen) return false;
  for (size_t k = j; k < line.size(); k++) {
    if (line[k] != ' ' && line[k] != '\t') return false;
  }
  return true;
}

bool isIndentedCode(std::string_view line, std::string_view* content) {
  if (line.size() >= 1 && line[0] == '\t') {
    *content = line.substr(1);
    return true;
  }
  if (line.size() >= 4 && line[0] == ' ' && line[1] == ' ' && line[2] == ' ' && line[3] == ' ') {
    *content = line.substr(4);
    return true;
  }
  return false;
}

size_t stripBlockquoteMarkers(std::string_view line, int* depth) {
  size_t i = 0;
  int d = 0;
  while (d < MAX_NEST_DEPTH) {
    size_t j = i;
    while (j < line.size() && (j - i) < 3 && line[j] == ' ') j++;
    if (j < line.size() && line[j] == '>') {
      j++;
      if (j < line.size() && line[j] == ' ') j++;
      i = j;
      d++;
    } else {
      break;
    }
  }
  *depth = d;
  return i;
}

bool parseListMarker(std::string_view line, bool* ordered, size_t* markerIndent, size_t* contentStart) {
  size_t i = 0;
  while (i < line.size() && i < 3 && line[i] == ' ') i++;
  *markerIndent = i;
  if (i >= line.size()) return false;

  if (line[i] == '-' || line[i] == '*' || line[i] == '+') {
    if (i + 1 == line.size()) {
      *ordered = false;
      *contentStart = line.size();
      return true;
    }
    if (line[i + 1] == ' ' || line[i + 1] == '\t') {
      *ordered = false;
      *contentStart = i + 1;
      while (*contentStart < line.size() && line[*contentStart] == ' ') (*contentStart)++;
      return true;
    }
    return false;
  }

  if (std::isdigit(static_cast<unsigned char>(line[i]))) {
    size_t j = i;
    while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j])) && (j - i) < 9) j++;
    if (j < line.size() && (line[j] == '.' || line[j] == ')') &&
        (j + 1 == line.size() || line[j + 1] == ' ' || line[j + 1] == '\t')) {
      *ordered = true;
      *contentStart = j + 1;
      while (*contentStart < line.size() && line[*contentStart] == ' ') (*contentStart)++;
      return true;
    }
  }
  return false;
}

bool parseImageOnlyLine(std::string_view line, std::string_view* alt, std::string_view* src) {
  size_t i = 0;
  while (i < line.size() && line[i] == ' ') i++;
  if (i >= line.size() || line[i] != '!') return false;
  i++;
  if (i >= line.size() || line[i] != '[') return false;
  i++;
  const size_t altEnd = line.find(']', i);
  if (altEnd == std::string_view::npos) return false;
  size_t j = altEnd + 1;
  if (j >= line.size() || line[j] != '(') return false;
  j++;
  const size_t srcEnd = line.find(')', j);
  if (srcEnd == std::string_view::npos) return false;
  for (size_t k = srcEnd + 1; k < line.size(); k++) {
    if (line[k] != ' ') return false;
  }
  *alt = line.substr(i, altEnd - i);
  *src = line.substr(j, srcEnd - j);
  return true;
}

// Validates a Markdown image reference resolves to a local, embeddable (JPEG/PNG), size-bounded
// file next to the source document. Rejects absolute paths, parent traversal, and remote URLs.
bool resolveLocalImage(const std::string& mdFolder, std::string_view relSrc, std::string* resolvedSdPath,
                       const char** ext) {
  if (relSrc.empty() || relSrc[0] == '/') return false;
  if (relSrc.find("://") != std::string_view::npos) return false;
  if (relSrc.find("..") != std::string_view::npos) return false;

  const std::string candidate =
      mdFolder + (!mdFolder.empty() && mdFolder.back() == '/' ? "" : "/") + std::string(relSrc);
  if (!Storage.exists(candidate.c_str())) return false;
  if (!FsHelpers::hasJpgExtension(candidate) && !FsHelpers::hasPngExtension(candidate)) return false;

  HalFile f;
  if (!Storage.openFileForRead("MDX", candidate, f)) return false;
  if (f.size() > MAX_IMAGE_BYTES) return false;

  *resolvedSdPath = candidate;
  *ext = FsHelpers::hasPngExtension(candidate) ? ".png" : ".jpg";
  return true;
}

enum class OpenKind : uint8_t { None, Paragraph, Blockquote, ListUl, ListOl, CodeFence, CodeIndented };

}  // namespace

MarkdownToXhtml::Result MarkdownToXhtml::convert(const std::string& srcPath, const std::string& dstPath) {
  Result result;

  HalFile in;
  if (!Storage.openFileForRead("MDX", srcPath, in)) {
    LOG_ERR("MDX", "Cannot open source: %s", srcPath.c_str());
    return result;
  }
  HalFile out;
  if (!Storage.openFileForWrite("MDX", dstPath, out)) {
    LOG_ERR("MDX", "Cannot open destination: %s", dstPath.c_str());
    return result;
  }

  LineSource lines(in);
  if (!lines.ok()) {
    LOG_ERR("MDX", "OOM allocating line buffer");
    return result;
  }

  const std::string mdFolder = FsHelpers::extractFolderPath(srcPath);
  Writer w{out};

  w.write(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<html "
      "xmlns=\"http://www.w3.org/1999/xhtml\"><head><title/></head><body>\n");

  OpenKind open = OpenKind::None;
  int quoteDepth = 0;
  int listDepth = 0;
  int listOrdinal[MAX_NEST_DEPTH + 1] = {};
  int lastListDepth = -1;
  char fenceChar = 0;
  int fenceLen = 0;
  bool pendingHardBreak = false;
  int headingCounter = 0;
  int imageCounter = 0;

  auto closeOpen = [&]() {
    switch (open) {
      case OpenKind::Paragraph:
        w.write("</p>\n");
        break;
      case OpenKind::Blockquote:
        w.write("</blockquote>\n");
        break;
      case OpenKind::ListUl:
        w.write("</li>\n");
        break;
      case OpenKind::ListOl:
        w.write("</div>\n");
        break;
      default:
        break;
    }
    open = OpenKind::None;
    pendingHardBreak = false;
  };

  std::string line;
  bool hardBreak = false;
  while (w.ok && lines.next(line, &hardBreak)) {
    const std::string_view v = line;

    // Fenced code content: every line is literal until the matching close fence, regardless
    // of what it would otherwise look like.
    if (open == OpenKind::CodeFence) {
      if (isFenceClose(v, fenceChar, fenceLen)) {
        open = OpenKind::None;
      } else {
        w.write("<p class=\"code\">");
        w.writeEscaped(v.empty() ? std::string_view(" ") : v);
        w.write("</p>\n");
      }
      continue;
    }

    if (isBlank(v)) {
      closeOpen();
      continue;
    }

    int headingLevel;
    std::string_view headingText;
    char newFenceChar;
    int newFenceLen;
    std::string_view codeContent;
    std::string_view imgAlt, imgSrc;
    bool ordered;
    size_t markerIndent, contentStart;
    int newQuoteDepth;

    if (parseAtxHeading(v, &headingLevel, &headingText)) {
      closeOpen();
      const std::string anchorId = "h" + std::to_string(headingCounter++);
      Heading heading{static_cast<uint8_t>(headingLevel), std::string(headingText), anchorId};
      if (headingLevel == 1 && result.firstH1.empty()) {
        result.firstH1 = heading.text;
      }
      result.headings.push_back(std::move(heading));

      w.write("<h");
      const char levelChar = static_cast<char>('0' + headingLevel);
      w.write(&levelChar, 1);
      w.write(" id=\"");
      w.write(anchorId);
      w.write("\">");
      if (headingLevel == 1) {
        renderInline(asciiUppercase(headingText), w);
      } else {
        renderInline(headingText, w);
      }
      w.write("</h");
      w.write(&levelChar, 1);
      w.write(">\n");
      if (headingLevel <= 2) {
        w.write("<hr/>\n");
      }
      continue;
    }

    if (isThematicBreak(v)) {
      closeOpen();
      w.write("<hr/>\n");
      continue;
    }

    if (parseFenceStart(v, &newFenceChar, &newFenceLen)) {
      closeOpen();
      fenceChar = newFenceChar;
      fenceLen = newFenceLen;
      open = OpenKind::CodeFence;
      continue;
    }

    if (open != OpenKind::Paragraph && isIndentedCode(v, &codeContent)) {
      if (open != OpenKind::CodeIndented) {
        closeOpen();
        open = OpenKind::CodeIndented;
      }
      w.write("<p class=\"code\">");
      w.writeEscaped(codeContent.empty() ? std::string_view(" ") : codeContent);
      w.write("</p>\n");
      continue;
    }
    if (open == OpenKind::CodeIndented) {
      closeOpen();
    }

    if (parseImageOnlyLine(v, &imgAlt, &imgSrc)) {
      closeOpen();
      std::string resolvedSdPath;
      const char* ext = nullptr;
      if (resolveLocalImage(mdFolder, imgSrc, &resolvedSdPath, &ext)) {
        std::string zipName = "OEBPS/images/img" + std::to_string(imageCounter++) + ext;
        w.write("<p><img src=\"images/");
        w.write(std::string_view(zipName).substr(sizeof("OEBPS/images/") - 1));
        w.write("\" alt=\"");
        w.writeEscaped(imgAlt);
        w.write("\"/></p>\n");
        result.images.push_back({std::move(resolvedSdPath), std::move(zipName)});
      } else {
        w.write("<p><em>[Image");
        if (!imgAlt.empty()) {
          w.write(": ");
          w.writeEscaped(imgAlt);
        }
        w.write("]</em></p>\n");
      }
      continue;
    }

    {
      size_t qContentOff = stripBlockquoteMarkers(v, &newQuoteDepth);
      if (newQuoteDepth > 0) {
        std::string_view remainder = v.substr(qContentOff);
        if (open != OpenKind::Blockquote || quoteDepth != newQuoteDepth) {
          closeOpen();
          quoteDepth = newQuoteDepth;
          open = OpenKind::Blockquote;
          w.write("<blockquote class=\"q");
          const char depthChar = static_cast<char>('0' + quoteDepth);
          w.write(&depthChar, 1);
          w.write("\">");
        } else {
          w.write(pendingHardBreak ? "<br/>" : " ");
        }
        renderInline(remainder, w);
        pendingHardBreak = hardBreak;
        continue;
      }
    }

    if (parseListMarker(v, &ordered, &markerIndent, &contentStart)) {
      closeOpen();
      listDepth = 1 + static_cast<int>(markerIndent / 2);
      if (listDepth > MAX_NEST_DEPTH) listDepth = MAX_NEST_DEPTH;
      if (listDepth != lastListDepth) {
        listOrdinal[listDepth] = 0;
      }
      lastListDepth = listDepth;
      listOrdinal[listDepth]++;
      const std::string_view itemText = v.substr(contentStart);
      const char depthChar = static_cast<char>('0' + listDepth);

      if (ordered) {
        open = OpenKind::ListOl;
        w.write("<div class=\"ol");
        w.write(&depthChar, 1);
        w.write("\">");
        w.write(std::to_string(listOrdinal[listDepth]));
        w.write(". ");
      } else {
        open = OpenKind::ListUl;
        w.write("<li class=\"ul");
        w.write(&depthChar, 1);
        w.write("\">");
      }
      renderInline(itemText, w);
      pendingHardBreak = hardBreak;
      continue;
    }

    // Plain text: continuation of the currently open block, or a new paragraph.
    if (open == OpenKind::Paragraph || open == OpenKind::Blockquote || open == OpenKind::ListUl ||
        open == OpenKind::ListOl) {
      w.write(pendingHardBreak ? "<br/>" : " ");
      renderInline(v, w);
      pendingHardBreak = hardBreak;
      continue;
    }

    closeOpen();
    open = OpenKind::Paragraph;
    w.write("<p>");
    renderInline(v, w);
    pendingHardBreak = hardBreak;
  }

  closeOpen();
  w.write("</body></html>\n");

  if (!w.ok) {
    LOG_ERR("MDX", "Write failure converting %s", srcPath.c_str());
    return result;
  }

  result.ok = true;
  result.crc32 = w.crc;
  result.size = w.size;
  return result;
}
