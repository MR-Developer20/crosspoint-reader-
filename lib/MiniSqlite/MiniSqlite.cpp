#include "MiniSqlite.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

constexpr int MAX_BTREE_DEPTH = 32;   // guards against corrupt/cyclic interior pages
constexpr uint32_t MAX_RECORD_HEADER = 512;

// Byte length of a value with the given SQLite serial type.
uint32_t serialSize(uint64_t st) {
  switch (st) {
    case 0:
    case 8:
    case 9:
    case 10:
    case 11:
      return 0;
    case 1:
      return 1;
    case 2:
      return 2;
    case 3:
      return 3;
    case 4:
      return 4;
    case 5:
      return 6;
    case 6:
    case 7:
      return 8;
    default:
      return static_cast<uint32_t>((st - 12) / 2);  // even = blob, odd = text
  }
}

// Decode a SQLite varint from an in-memory buffer. Returns bytes consumed (1..9) or 0.
int decodeVarint(const uint8_t* buf, uint32_t avail, uint64_t& value) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    if (static_cast<uint32_t>(i) >= avail) {
      return 0;
    }
    v = (v << 7) | (buf[i] & 0x7F);
    if ((buf[i] & 0x80) == 0) {
      value = v;
      return i + 1;
    }
  }
  if (avail < 9) {
    return 0;
  }
  v = (v << 8) | buf[8];
  value = v;
  return 9;
}

bool charEqualsIgnoreCase(char a, char b) {
  return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
}

bool equalsIgnoreCase(const std::string& a, const char* b) {
  size_t i = 0;
  for (; i < a.size(); i++) {
    if (b[i] == '\0' || !charEqualsIgnoreCase(a[i], b[i])) {
      return false;
    }
  }
  return b[i] == '\0';
}

bool containsIgnoreCase(const std::string& haystack, const char* needle) {
  const size_t nlen = std::strlen(needle);
  if (nlen == 0 || haystack.size() < nlen) {
    return false;
  }
  for (size_t i = 0; i + nlen <= haystack.size(); i++) {
    size_t j = 0;
    for (; j < nlen; j++) {
      if (!charEqualsIgnoreCase(haystack[i + j], needle[j])) {
        break;
      }
    }
    if (j == nlen) {
      return true;
    }
  }
  return false;
}

// First token of a column/constraint definition, matched case-insensitively against the
// SQLite table-level constraint keywords. Such segments are not columns.
bool isTableConstraintKeyword(const std::string& word) {
  static const char* kKeywords[] = {"CONSTRAINT", "PRIMARY", "UNIQUE", "CHECK", "FOREIGN", "KEY"};
  for (const char* kw : kKeywords) {
    if (equalsIgnoreCase(word, kw)) {
      return true;
    }
  }
  return false;
}

// Pull the first identifier out of a column-definition segment. Handles the SQLite quoting
// styles ("x", `x`, [x]) and plain identifiers. `rest` receives everything after the name.
void extractFirstIdentifier(const std::string& seg, std::string& name, std::string& rest) {
  name.clear();
  rest.clear();
  size_t i = 0;
  while (i < seg.size() && std::isspace(static_cast<unsigned char>(seg[i]))) {
    i++;
  }
  if (i >= seg.size()) {
    return;
  }
  const char c = seg[i];
  if (c == '"' || c == '`' || c == '[') {
    const char close = (c == '[') ? ']' : c;
    i++;
    while (i < seg.size() && seg[i] != close) {
      name.push_back(seg[i]);
      i++;
    }
    if (i < seg.size()) {
      i++;  // skip closing quote
    }
  } else {
    while (i < seg.size() && !std::isspace(static_cast<unsigned char>(seg[i])) && seg[i] != '(') {
      name.push_back(seg[i]);
      i++;
    }
  }
  rest = seg.substr(i);
}

// Extract ordered column names from a CREATE TABLE statement, skipping table-level
// constraints, and note which column (if any) is INTEGER PRIMARY KEY (a rowid alias).
void parseColumnsFromCreateSql(const std::string& sql, std::vector<std::string>& cols, int& rowidAlias) {
  cols.clear();
  rowidAlias = -1;

  const size_t open = sql.find('(');
  if (open == std::string::npos) {
    return;
  }

  // Find the matching close paren for the column-list.
  int depth = 0;
  size_t end = std::string::npos;
  for (size_t i = open; i < sql.size(); i++) {
    if (sql[i] == '(') {
      depth++;
    } else if (sql[i] == ')') {
      depth--;
      if (depth == 0) {
        end = i;
        break;
      }
    }
  }
  if (end == std::string::npos) {
    end = sql.size();
  }

  size_t segStart = open + 1;
  depth = 0;
  int colIndex = 0;
  for (size_t p = open + 1; p <= end; p++) {
    if (p < end) {
      const char ch = sql[p];
      if (ch == '(') {
        depth++;
        continue;
      }
      if (ch == ')') {
        if (depth > 0) {
          depth--;
        }
        continue;
      }
      if (!(depth == 0 && ch == ',')) {
        continue;
      }
    }
    // Reached a top-level comma (or the final close paren): flush [segStart, p).
    const std::string seg = sql.substr(segStart, p - segStart);
    std::string colName;
    std::string rest;
    extractFirstIdentifier(seg, colName, rest);
    if (!colName.empty() && !isTableConstraintKeyword(colName)) {
      if (containsIgnoreCase(rest, "integer") && containsIgnoreCase(rest, "primary") &&
          containsIgnoreCase(rest, "key")) {
        rowidAlias = colIndex;
      }
      cols.push_back(std::move(colName));
      colIndex++;
    }
    segStart = p + 1;
  }
}

}  // namespace

int MiniSqlite::TableInfo::columnIndex(const char* name) const {
  for (size_t i = 0; i < columns.size(); i++) {
    if (equalsIgnoreCase(columns[i], name)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool MiniSqlite::open(const std::string& path) {
  if (!Storage.openFileForRead("MINISQL", path, file_)) {
    LOG_ERR("MINISQL", "open failed: %s", path.c_str());
    return false;
  }

  uint8_t hdr[100];
  if (file_.read(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) {
    LOG_ERR("MINISQL", "short header");
    return false;
  }
  if (memcmp(hdr, "SQLite format 3\0", 16) != 0) {
    LOG_ERR("MINISQL", "bad magic");
    return false;
  }

  uint32_t ps = (static_cast<uint32_t>(hdr[16]) << 8) | hdr[17];
  if (ps == 1) {
    ps = 65536;  // per spec, a stored page size of 1 means 65536
  }
  if (ps < 512 || (ps & (ps - 1)) != 0) {
    LOG_ERR("MINISQL", "bad page size %u", ps);
    return false;
  }
  pageSize_ = ps;
  usableSize_ = ps - hdr[20];  // byte 20 = bytes of unused reserved space at end of each page
  textEncoding_ = hdr[59];     // 4-byte BE int at 56..59; value 1=UTF-8, 2/3=UTF-16
  if (textEncoding_ != 1) {
    LOG_INF("MINISQL", "non-UTF8 text encoding %u; text columns may be misread", textEncoding_);
  }
  LOG_DBG("MINISQL", "opened %s: pageSize=%u usable=%u", path.c_str(), pageSize_, usableSize_);
  return true;
}

bool MiniSqlite::readAt(uint64_t offset, void* buf, size_t len) {
  if (!file_.seek64(offset)) {
    return false;
  }
  return file_.read(buf, len) == static_cast<int>(len);
}

bool MiniSqlite::readU16BE(uint64_t offset, uint16_t& out) {
  uint8_t b[2];
  if (!readAt(offset, b, 2)) {
    return false;
  }
  out = static_cast<uint16_t>((b[0] << 8) | b[1]);
  return true;
}

bool MiniSqlite::readU32BE(uint64_t offset, uint32_t& out) {
  uint8_t b[4];
  if (!readAt(offset, b, 4)) {
    return false;
  }
  out = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
        (static_cast<uint32_t>(b[2]) << 8) | b[3];
  return true;
}

int MiniSqlite::readVarint(uint64_t offset, uint64_t& value) {
  uint8_t b[9];
  if (!file_.seek64(offset)) {
    return 0;
  }
  const int n = file_.read(b, sizeof(b));
  if (n <= 0) {
    return 0;
  }
  return decodeVarint(b, static_cast<uint32_t>(n), value);
}

MiniSqlite::Cursor MiniSqlite::scan(uint32_t rootPage) {
  Cursor c(this);
  c.pushPage(rootPage);  // on failure ok_ is set false and next() returns false immediately
  return c;
}

bool MiniSqlite::findTable(const char* tableName, TableInfo& out) {
  // sqlite_schema (root page 1) columns: 0=type, 1=name, 2=tbl_name, 3=rootpage, 4=sql.
  Cursor c = scan(1);
  while (c.next()) {
    std::string type;
    if (!c.getText(0, type) || !equalsIgnoreCase(type, "table")) {
      continue;
    }
    std::string name;
    if (!c.getText(1, name) || !equalsIgnoreCase(name, tableName)) {
      continue;
    }
    int64_t root = 0;
    if (!c.getInt(3, root) || root <= 0) {
      LOG_ERR("MINISQL", "table %s has no rootpage", tableName);
      return false;
    }
    std::string sql;
    c.getText(4, sql);
    out.rootPage = static_cast<uint32_t>(root);
    parseColumnsFromCreateSql(sql, out.columns, out.rowidAlias);
    LOG_DBG("MINISQL", "found table %s root=%u cols=%u", tableName, out.rootPage,
            static_cast<unsigned>(out.columns.size()));
    return true;
  }
  return false;
}

bool MiniSqlite::Cursor::pushPage(uint32_t page) {
  if (page == 0 || stack_.size() >= MAX_BTREE_DEPTH) {
    ok_ = false;
    return false;
  }
  Frame f;
  f.page = page;
  f.headerOffset = (page == 1) ? 100 : 0;
  const uint64_t hdr = db_->pageBaseOf(page) + f.headerOffset;

  uint8_t type;
  if (!db_->readAt(hdr, &type, 1)) {
    ok_ = false;
    return false;
  }
  f.type = type;
  if (type != 0x05 && type != 0x0d) {
    // Only ordinary table b-tree pages are expected (0x02/0x0a index pages are not scanned).
    LOG_ERR("MINISQL", "unexpected page type 0x%02x on page %u", type, page);
    ok_ = false;
    return false;
  }
  if (!db_->readU16BE(hdr + 3, f.numCells)) {
    ok_ = false;
    return false;
  }
  if (type == 0x05 && !db_->readU32BE(hdr + 8, f.rightPtr)) {
    ok_ = false;
    return false;
  }
  stack_.push_back(f);
  return true;
}

bool MiniSqlite::Cursor::next() {
  if (!ok_) {
    return false;
  }
  while (!stack_.empty()) {
    Frame& f = stack_.back();
    const uint64_t base = db_->pageBaseOf(f.page);
    const uint16_t cellArray = f.headerOffset + (f.type == 0x05 ? 12 : 8);

    if (f.type == 0x0d) {  // leaf: cells are rows
      if (f.nextCell < f.numCells) {
        uint16_t cellPtr;
        if (!db_->readU16BE(base + cellArray + static_cast<uint64_t>(f.nextCell) * 2, cellPtr)) {
          ok_ = false;
          return false;
        }
        f.nextCell++;
        if (!parseCurrentCell(cellPtr)) {
          ok_ = false;
          return false;
        }
        return true;
      }
      stack_.pop_back();
    } else {  // interior 0x05: descend into children left-to-right, then the right pointer
      if (f.nextCell < f.numCells) {
        uint16_t cellPtr;
        if (!db_->readU16BE(base + cellArray + static_cast<uint64_t>(f.nextCell) * 2, cellPtr)) {
          ok_ = false;
          return false;
        }
        f.nextCell++;
        uint32_t child;
        if (!db_->readU32BE(base + cellPtr, child)) {  // interior cell begins with the left child page
          ok_ = false;
          return false;
        }
        if (!pushPage(child)) {
          return false;  // ok_ already set; note: `f` is now dangling — do not touch it
        }
      } else if (!f.rightVisited) {
        f.rightVisited = true;
        const uint32_t right = f.rightPtr;  // copy before pushPage may reallocate the stack
        if (!pushPage(right)) {
          return false;
        }
      } else {
        stack_.pop_back();
      }
    }
  }
  return false;  // traversal complete
}

bool MiniSqlite::Cursor::parseCurrentCell(uint32_t cellPtr) {
  const uint32_t page = stack_.back().page;
  const uint64_t base = db_->pageBaseOf(page);

  uint64_t payloadLen = 0;
  const int c1 = db_->readVarint(base + cellPtr, payloadLen);
  if (c1 == 0) {
    return false;
  }
  uint64_t rowid = 0;
  const int c2 = db_->readVarint(base + cellPtr + c1, rowid);
  if (c2 == 0) {
    return false;
  }

  cur_.page = page;
  cur_.payloadStart = cellPtr + c1 + c2;
  cur_.total = payloadLen;
  cur_.rowid = static_cast<int64_t>(rowid);
  cur_.firstOverflow = 0;

  // Local-payload length for a table-leaf cell (SQLite spec).
  const uint32_t U = db_->usableSize_;
  const uint32_t X = U - 35;
  if (payloadLen <= X) {
    cur_.localLen = static_cast<uint32_t>(payloadLen);
  } else {
    const uint32_t M = ((U - 12) * 32 / 255) - 23;
    const uint32_t K = M + static_cast<uint32_t>((payloadLen - M) % (U - 4));
    cur_.localLen = (K <= X) ? K : M;
    if (!db_->readU32BE(base + cur_.payloadStart + cur_.localLen, cur_.firstOverflow)) {
      return false;
    }
  }
  return parseRecordHeader();
}

bool MiniSqlite::Cursor::parseRecordHeader() {
  numCols_ = 0;
  if (cur_.total == 0) {
    return true;
  }

  // The record header begins with a varint giving the header's own byte length.
  uint8_t lead[9];
  const uint32_t leadRead = static_cast<uint32_t>(std::min<uint64_t>(sizeof(lead), cur_.total));
  if (!readPayload(0, leadRead, lead)) {
    return false;
  }
  uint64_t headerLen = 0;
  const int hc = decodeVarint(lead, leadRead, headerLen);
  if (hc == 0 || headerLen > MAX_RECORD_HEADER || headerLen > cur_.total) {
    return false;
  }

  uint8_t hbuf[MAX_RECORD_HEADER];
  if (!readPayload(0, static_cast<uint32_t>(headerLen), hbuf)) {
    return false;
  }

  uint32_t pos = static_cast<uint32_t>(hc);
  uint32_t bodyOffset = static_cast<uint32_t>(headerLen);  // body starts right after the header
  int col = 0;
  while (pos < headerLen && col < MAX_COLUMNS) {
    uint64_t st = 0;
    const int used = decodeVarint(hbuf + pos, static_cast<uint32_t>(headerLen) - pos, st);
    if (used == 0) {
      return false;
    }
    pos += static_cast<uint32_t>(used);
    serialCode_[col] = static_cast<uint32_t>(st);
    colOffset_[col] = bodyOffset;
    colSize_[col] = serialSize(st);
    bodyOffset += colSize_[col];
    col++;
  }
  numCols_ = col;
  return true;
}

bool MiniSqlite::Cursor::readPayload(uint64_t logicalOffset, uint32_t length, uint8_t* dest) const {
  if (length == 0) {
    return true;
  }
  if (logicalOffset + length > cur_.total) {
    return false;
  }

  const uint32_t U = db_->usableSize_;
  uint32_t remaining = length;

  // Local (on-page) portion.
  if (logicalOffset < cur_.localLen) {
    const uint32_t fromLocal = static_cast<uint32_t>(std::min<uint64_t>(remaining, cur_.localLen - logicalOffset));
    if (!db_->readAt(db_->pageBaseOf(cur_.page) + cur_.payloadStart + logicalOffset, dest, fromLocal)) {
      return false;
    }
    dest += fromLocal;
    remaining -= fromLocal;
    logicalOffset += fromLocal;
  }
  if (remaining == 0) {
    return true;
  }

  // Overflow-chain portion. Each overflow page is a 4-byte next-page pointer followed by
  // (usableSize - 4) content bytes.
  const uint32_t ovContent = U - 4;
  uint64_t ovLogical = logicalOffset - cur_.localLen;  // offset within the overflow region
  uint32_t ovPage = cur_.firstOverflow;
  while (ovPage != 0 && ovLogical >= ovContent) {
    uint32_t nextPage;
    if (!db_->readU32BE(db_->pageBaseOf(ovPage), nextPage)) {
      return false;
    }
    ovPage = nextPage;
    ovLogical -= ovContent;
  }
  while (remaining > 0 && ovPage != 0) {
    const uint32_t avail = ovContent - static_cast<uint32_t>(ovLogical);
    const uint32_t chunk = std::min(remaining, avail);
    if (!db_->readAt(db_->pageBaseOf(ovPage) + 4 + ovLogical, dest, chunk)) {
      return false;
    }
    dest += chunk;
    remaining -= chunk;
    ovLogical = 0;
    if (remaining > 0) {
      uint32_t nextPage;
      if (!db_->readU32BE(db_->pageBaseOf(ovPage), nextPage)) {
        return false;
      }
      ovPage = nextPage;
    }
  }
  return remaining == 0;
}

bool MiniSqlite::Cursor::isNull(int col) const {
  if (col < 0 || col >= numCols_) {
    return true;
  }
  return serialCode_[col] == 0;
}

bool MiniSqlite::Cursor::getInt(int col, int64_t& out) const {
  if (col < 0 || col >= numCols_) {
    return false;
  }
  const uint32_t st = serialCode_[col];
  if (st == 8) {
    out = 0;
    return true;
  }
  if (st == 9) {
    out = 1;
    return true;
  }
  if (st < 1 || st > 6) {
    return false;  // NULL, float, text, blob, or reserved
  }
  uint8_t b[8];
  const uint32_t n = colSize_[col];
  if (!readPayload(colOffset_[col], n, b)) {
    return false;
  }
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; i++) {
    v = (v << 8) | b[i];
  }
  if (n < 8) {  // sign-extend from the n-byte big-endian value
    const uint64_t signBit = 1ull << (n * 8 - 1);
    if (v & signBit) {
      v |= ~((1ull << (n * 8)) - 1);
    }
  }
  out = static_cast<int64_t>(v);
  return true;
}

bool MiniSqlite::Cursor::getText(int col, std::string& out) const {
  out.clear();
  if (col < 0 || col >= numCols_) {
    return false;
  }
  const uint32_t st = serialCode_[col];
  if (st < 13 || (st % 2) == 0) {
    return false;  // must be a text serial type (odd, >= 13)
  }
  const uint32_t n = colSize_[col];
  if (n == 0) {
    return true;  // empty string
  }
  out.resize(n);
  if (!readPayload(colOffset_[col], n, reinterpret_cast<uint8_t*>(&out[0]))) {
    out.clear();
    return false;
  }
  return true;
}

std::unique_ptr<uint8_t[]> MiniSqlite::Cursor::getColumnBytes(int col, size_t* size) const {
  if (col < 0 || col >= numCols_) {
    return nullptr;
  }
  const uint32_t st = serialCode_[col];
  if (st < 12) {
    return nullptr;  // NULL / int / float / reserved — not a byte payload
  }
  const uint32_t n = colSize_[col];
  auto buf = makeUniqueNoThrow<uint8_t[]>(n == 0 ? 1 : n);
  if (!buf) {
    LOG_ERR("MINISQL", "OOM reading column %d (%u bytes)", col, n);
    return nullptr;
  }
  if (n > 0 && !readPayload(colOffset_[col], n, buf.get())) {
    return nullptr;
  }
  if (size) {
    *size = n;
  }
  return buf;
}
