#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Minimal, read-only SQLite reader for the constrained target.
//
// Supports exactly what JWPUB needs and nothing more: full-table scans of ordinary
// (rowid) table b-trees, decoding the record format (ints / text / blobs) and following
// overflow-page chains for large values (e.g. Document.Content). No SQL, no indexes, no
// writes, no query planner, no journal/WAL.
//
// The b-tree is walked with an explicit frame stack and every value is read straight from
// the file via HalFile random access, so peak RAM is a handful of small buffers regardless
// of database size or row size — a whole page is never buffered. This mirrors the project's
// hand-rolled ZipFile rather than pulling in the full sqlite3 amalgamation, and keeps us
// inside the RAM ceiling.
//
// The referenced database file must outlive any Cursor obtained from it.
class MiniSqlite {
 public:
  static constexpr int MAX_COLUMNS = 64;

  MiniSqlite() = default;

  // Open a .db file and read its header (page size / usable size / text encoding).
  bool open(const std::string& path);
  bool isOpen() const { return static_cast<bool>(file_); }

  struct TableInfo {
    uint32_t rootPage = 0;
    std::vector<std::string> columns;  // column names in definition order
    int rowidAlias = -1;               // index of the INTEGER PRIMARY KEY column (== rowid), or -1
    // Case-insensitive column lookup. Returns the column index, or -1 if absent.
    int columnIndex(const char* name) const;
  };

  // Locate a table in sqlite_schema (root page 1) and parse its column list from the stored
  // CREATE TABLE statement. Returns false if the table is not found.
  bool findTable(const char* tableName, TableInfo& out);

  // Forward-only scan over a table b-tree. Obtain one via MiniSqlite::scan().
  class Cursor {
   public:
    // Advance to the next row. Returns false when the scan is finished OR a read error
    // occurred; call ok() to distinguish. The column accessors below are valid only for the
    // current row, until the next call to next().
    bool next();
    bool ok() const { return ok_; }

    int64_t rowid() const { return cur_.rowid; }
    bool isNull(int col) const;
    // Integer column. Returns false if the column is NULL, out of range, or not an integer.
    // INTEGER PRIMARY KEY columns are stored as NULL in the record; read those via rowid().
    bool getInt(int col, int64_t& out) const;
    // Text column as bytes into `out` (NOT null-terminated content-wise, but std::string adds
    // its own terminator). Returns false on NULL / non-text / read error.
    bool getText(int col, std::string& out) const;
    // Read a text/blob column into a freshly allocated heap buffer, following overflow pages.
    // Returns nullptr on NULL / non-blob-or-text / OOM / read error; sets *size on success.
    std::unique_ptr<uint8_t[]> getColumnBytes(int col, size_t* size) const;

   private:
    friend class MiniSqlite;
    explicit Cursor(MiniSqlite* db) : db_(db) {}

    struct Frame {
      uint32_t page = 0;
      uint16_t headerOffset = 0;  // 100 on page 1 (skips the DB header), else 0
      uint8_t type = 0;           // 0x05 interior table, 0x0d leaf table
      uint16_t numCells = 0;
      uint16_t nextCell = 0;
      uint32_t rightPtr = 0;  // interior pages only
      bool rightVisited = false;
    };

    // Payload location of the current leaf cell.
    struct CellPayload {
      uint32_t page = 0;
      uint32_t payloadStart = 0;  // byte offset within the page, after the two cell varints
      uint64_t total = 0;         // P: total payload byte length
      uint32_t localLen = 0;      // bytes of payload stored on this page
      uint32_t firstOverflow = 0;
      int64_t rowid = 0;
    };

    bool pushPage(uint32_t page);
    bool parseCurrentCell(uint32_t cellPtr);
    bool parseRecordHeader();
    // Copy `length` payload bytes starting at logical payload offset `logicalOffset` into
    // `dest`, spanning the local portion and the overflow chain as needed.
    bool readPayload(uint64_t logicalOffset, uint32_t length, uint8_t* dest) const;

    MiniSqlite* db_;
    std::vector<Frame> stack_;
    bool ok_ = true;

    CellPayload cur_;
    int numCols_ = 0;
    uint32_t serialCode_[MAX_COLUMNS] = {};  // raw serial-type code of each column
    uint32_t colOffset_[MAX_COLUMNS] = {};   // logical payload offset of each column's data
    uint32_t colSize_[MAX_COLUMNS] = {};     // byte length of each column's data
  };

  // Begin a full scan of the table b-tree rooted at `rootPage`.
  Cursor scan(uint32_t rootPage);

  uint32_t pageSize() const { return pageSize_; }

 private:
  friend class Cursor;

  uint64_t pageBaseOf(uint32_t page) const { return static_cast<uint64_t>(page - 1) * pageSize_; }

  bool readAt(uint64_t offset, void* buf, size_t len);
  bool readU16BE(uint64_t offset, uint16_t& out);
  bool readU32BE(uint64_t offset, uint32_t& out);
  // Decode a SQLite varint at file offset `offset`. Returns bytes consumed (1..9), or 0 on error.
  int readVarint(uint64_t offset, uint64_t& value);

  HalFile file_;
  uint32_t pageSize_ = 0;
  uint32_t usableSize_ = 0;  // pageSize - per-page reserved bytes
  uint8_t textEncoding_ = 1;
};
