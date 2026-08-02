#include "ScrambleGame.h"

#include <Arduino.h>

#include <cctype>
#include <cstdio>
#include <cstring>

bool ScrambleGame::load(const std::string& path) {
  return PackLoader::loadWord(path, words, WORD_CAP, packName, sizeof(packName));
}

bool ScrambleGame::equalsIgnoreCase(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

void ScrambleGame::scramble(const char* src, char* dst, size_t dstLen) {
  size_t n = strnlen(src, dstLen - 1);
  memcpy(dst, src, n);
  dst[n] = '\0';
  if (n < 2) return;
  // Fisher-Yates; retry a few times if it lands back on the original.
  for (int attempt = 0; attempt < 6; ++attempt) {
    for (size_t i = n - 1; i > 0; --i) {
      size_t j = static_cast<size_t>(random(0, static_cast<long>(i) + 1));
      char tmp = dst[i];
      dst[i] = dst[j];
      dst[j] = tmp;
    }
    if (!equalsIgnoreCase(dst, src)) break;
  }
}

void ScrambleGame::onRoundStart(int round) {
  roundIndex = round;
  revealing = false;
  anySolved = false;
  rankNext = 0;
  for (int i = 0; i < ArcadeSession::MAX_CLIENTS; ++i) solved[i] = false;
  scramble(words[round].word, scrambled, sizeof(scrambled));
  deadline = millis() + static_cast<uint32_t>(DUR_SECONDS) * 1000;
}

bool ScrambleGame::onClientIntent(uint8_t pid, const JsonDocument& msg) {
  if (revealing || pid >= ArcadeSession::MAX_CLIENTS) return false;
  if (strcmp(msg["t"] | "", "guess") != 0) return false;
  // Route by fields present: scramble owns guess{text}; gc owns guess{r,g,b}.
  if (!msg["text"].is<const char*>()) return false;
  if (solved[pid]) return false;
  const char* g = msg["text"] | "";
  if (!equalsIgnoreCase(g, words[roundIndex].word)) return false;  // wrong — no rebroadcast
  solved[pid] = true;
  anySolved = true;
  uint32_t pts = (rankNext < 4) ? RANK_POINTS[rankNext] : 0;
  rankNext++;
  session->awardPoints(pid, pts);
  return true;
}

int ScrambleGame::solvedCount() const {
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  int s = 0;
  for (int i = 0; i < n; ++i)
    if (solved[pids[i]]) s++;
  return s;
}

bool ScrambleGame::roundComplete() const {
  if (millis() >= deadline) return true;
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  if (n == 0) return false;
  for (int i = 0; i < n; ++i)
    if (!solved[pids[i]]) return false;
  return true;
}

void ScrambleGame::onReveal() { revealing = true; }  // scores already awarded on solve

void ScrambleGame::buildStateFor(uint8_t pid, JsonDocument& out) {
  out["t"] = "scramble";
  out["phase"] = revealing ? "reveal" : "play";
  out["round"] = roundIndex;
  out["rounds"] = roundCount();
  out["scram"] = scrambled;
  out["len"] = static_cast<int>(strlen(words[roundIndex].word));
  out["solved"] = (pid < ArcadeSession::MAX_CLIENTS) ? solved[pid] : false;
  out["deadline"] = deadline;
  out["dur"] = DUR_SECONDS;
  if (revealing) out["word"] = words[roundIndex].word;
  session->fillScores(out);
}

void ScrambleGame::buildFinal(JsonDocument& out) {
  out["t"] = "scramble";
  out["phase"] = "final";
  session->fillBoard(out);
}

void ScrambleGame::roundLabel(char* buf, int len) const {
  snprintf(buf, len, "Word %d / %d", roundIndex + 1, roundCount());
}

void ScrambleGame::tallyLine(char* buf, int len) const {
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  snprintf(buf, len, "Solved %d/%d", solvedCount(), n);
}
