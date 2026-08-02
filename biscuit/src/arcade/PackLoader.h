#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// SD pack loaders for the arcade games. Byte-compatible with the Flipper
// Hotspot Arcade pack files (drop-in). Same fixed-field, no-String line reader
// as FlashcardActivity — blocks separated by a line containing only "---",
// first "Pack: <name>" line is the display title.
//
// Three content shapes:
//   TRIVIA : Q / A / B / C / D / Answer
//   AB     : A / B                       (Would You Rather)
//   WORD   : Word                         (Word Scramble)
namespace PackLoader {

struct TriviaQ {
  char q[192];
  char opt[4][64];
  uint8_t correct;  // 0..3
};

struct AbItem {
  char a[96];
  char b[96];
};

struct WordItem {
  char word[32];
};

// List *.txt files in dir (full paths). Creates dir if missing.
std::vector<std::string> scanPacks(const char* dir);

// Read the "Pack:" title of a file (fallback = filename). buf is null-terminated.
void readTitle(const std::string& path, char* buf, size_t bufLen);

// Loaders. `cap` bounds memory; extra blocks are skipped (logged). Return false
// if the file cannot be opened or yields no items. `title` optional (may be null).
bool loadTrivia(const std::string& path, std::vector<TriviaQ>& out, size_t cap, char* title, size_t titleLen);
bool loadAb(const std::string& path, std::vector<AbItem>& out, size_t cap, char* title, size_t titleLen);
bool loadWord(const std::string& path, std::vector<WordItem>& out, size_t cap, char* title, size_t titleLen);

}  // namespace PackLoader
