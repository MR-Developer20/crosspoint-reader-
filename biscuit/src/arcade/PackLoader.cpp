#include "PackLoader.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace {

// Read one '\n'-terminated line into buf (byte-by-byte, no String churn).
// Strips a trailing '\r'. Returns false at EOF with nothing read.
bool readLine(HalFile& f, char* buf, int cap) {
  int len = 0;
  bool any = false;
  while (f.available() && len < cap - 1) {
    char c = (char)f.read();
    any = true;
    if (c == '\n') break;
    buf[len++] = c;
  }
  if (len > 0 && buf[len - 1] == '\r') len--;
  buf[len] = '\0';
  return any || len > 0;
}

// Match "Key: value" (case-sensitive key). Returns pointer to value (trimmed
// leading spaces) or nullptr.
const char* field(const char* line, const char* key) {
  size_t kl = strlen(key);
  if (strncmp(line, key, kl) != 0) return nullptr;
  const char* v = line + kl;
  while (*v == ' ') v++;
  return v;
}

void copyField(char* dst, size_t dstLen, const char* src) {
  strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = '\0';
}

}  // namespace

namespace PackLoader {

std::vector<std::string> scanPacks(const char* dir) {
  std::vector<std::string> out;
  Storage.mkdir(dir);
  HalFile d = Storage.open(dir);
  if (!d) return out;
  HalFile e;
  while ((e = d.openNextFile())) {
    char name[64];
    e.getName(name, sizeof(name));
    bool isDir = e.isDirectory();
    e.close();
    if (isDir) continue;
    std::string n = name;
    if (n.size() > 4 && n.substr(n.size() - 4) == ".txt") out.push_back(std::string(dir) + "/" + n);
  }
  d.close();
  return out;
}

void readTitle(const std::string& path, char* buf, size_t bufLen) {
  // Default to the filename (without extension).
  size_t slash = path.rfind('/');
  std::string fn = (slash == std::string::npos) ? path : path.substr(slash + 1);
  if (fn.size() > 4 && fn.substr(fn.size() - 4) == ".txt") fn = fn.substr(0, fn.size() - 4);
  copyField(buf, bufLen, fn.c_str());

  HalFile f = Storage.open(path.c_str());
  if (!f) return;
  char line[200];
  while (readLine(f, line, sizeof(line))) {
    const char* v = field(line, "Pack:");
    if (v) {
      copyField(buf, bufLen, v);
      break;
    }
    if (!f.available()) break;
  }
  f.close();
}

bool loadTrivia(const std::string& path, std::vector<TriviaQ>& out, size_t cap, char* title, size_t titleLen) {
  out.clear();
  out.reserve(cap);
  if (title) readTitle(path, title, titleLen);

  HalFile f = Storage.open(path.c_str());
  if (!f) return false;

  TriviaQ cur;
  memset(&cur, 0, sizeof(cur));
  bool haveQ = false;
  int skipped = 0;
  char line[256];

  auto flush = [&]() {
    if (haveQ) {
      if (out.size() < cap)
        out.push_back(cur);
      else
        skipped++;
    }
    memset(&cur, 0, sizeof(cur));
    haveQ = false;
  };

  while (readLine(f, line, sizeof(line))) {
    if (strcmp(line, "---") == 0) {
      flush();
      continue;
    }
    const char* v;
    if ((v = field(line, "Q:"))) {
      copyField(cur.q, sizeof(cur.q), v);
      haveQ = true;
    } else if ((v = field(line, "A:"))) {
      copyField(cur.opt[0], sizeof(cur.opt[0]), v);
    } else if ((v = field(line, "B:"))) {
      copyField(cur.opt[1], sizeof(cur.opt[1]), v);
    } else if ((v = field(line, "C:"))) {
      copyField(cur.opt[2], sizeof(cur.opt[2]), v);
    } else if ((v = field(line, "D:"))) {
      copyField(cur.opt[3], sizeof(cur.opt[3]), v);
    } else if ((v = field(line, "Answer:"))) {
      char c = v[0];
      if (c >= 'a' && c <= 'd') c = static_cast<char>(c - 'a' + 'A');
      cur.correct = (c >= 'A' && c <= 'D') ? static_cast<uint8_t>(c - 'A') : 0;
    }
    if (!f.available()) break;
  }
  flush();
  f.close();
  if (skipped) LOG_INF("ARC", "Trivia pack capped: skipped %d question(s) over %u", skipped, (unsigned)cap);
  return !out.empty();
}

bool loadAb(const std::string& path, std::vector<AbItem>& out, size_t cap, char* title, size_t titleLen) {
  out.clear();
  out.reserve(cap);
  if (title) readTitle(path, title, titleLen);

  HalFile f = Storage.open(path.c_str());
  if (!f) return false;

  AbItem cur;
  memset(&cur, 0, sizeof(cur));
  bool have = false;
  int skipped = 0;
  char line[200];

  auto flush = [&]() {
    if (have) {
      if (out.size() < cap)
        out.push_back(cur);
      else
        skipped++;
    }
    memset(&cur, 0, sizeof(cur));
    have = false;
  };

  while (readLine(f, line, sizeof(line))) {
    if (strcmp(line, "---") == 0) {
      flush();
      continue;
    }
    const char* v;
    if ((v = field(line, "A:"))) {
      copyField(cur.a, sizeof(cur.a), v);
      have = true;
    } else if ((v = field(line, "B:"))) {
      copyField(cur.b, sizeof(cur.b), v);
      have = true;
    }
    if (!f.available()) break;
  }
  flush();
  f.close();
  if (skipped) LOG_INF("ARC", "WYR pack capped: skipped %d", skipped);
  return !out.empty();
}

bool loadWord(const std::string& path, std::vector<WordItem>& out, size_t cap, char* title, size_t titleLen) {
  out.clear();
  out.reserve(cap);
  if (title) readTitle(path, title, titleLen);

  HalFile f = Storage.open(path.c_str());
  if (!f) return false;

  int skipped = 0;
  char line[128];
  while (readLine(f, line, sizeof(line))) {
    const char* v = field(line, "Word:");
    if (v && v[0]) {
      if (out.size() < cap) {
        WordItem w;
        memset(&w, 0, sizeof(w));
        copyField(w.word, sizeof(w.word), v);
        out.push_back(w);
      } else {
        skipped++;
      }
    }
    if (!f.available()) break;
  }
  f.close();
  if (skipped) LOG_INF("ARC", "Scramble pack capped: skipped %d", skipped);
  return !out.empty();
}

}  // namespace PackLoader
