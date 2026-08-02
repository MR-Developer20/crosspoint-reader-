#include "WyrGame.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

bool WyrGame::load(const std::string& path) {
  return PackLoader::loadAb(path, prompts, PROMPT_CAP, packName, sizeof(packName));
}

void WyrGame::onRoundStart(int round) {
  roundIndex = round;
  revealing = false;
  for (int i = 0; i < ArcadeSession::MAX_CLIENTS; ++i) vote[i] = -1;
  counts[0] = counts[1] = 0;
  deadline = millis() + SOFT_DEADLINE_MS;
}

bool WyrGame::onClientIntent(uint8_t pid, const JsonDocument& msg) {
  if (revealing || pid >= ArcadeSession::MAX_CLIENTS) return false;
  if (strcmp(msg["t"] | "", "answer") != 0) return false;
  int c = msg["c"] | -1;
  if (c < 0 || c > 1) return false;
  if (vote[pid] == c) return false;
  if (vote[pid] >= 0 && counts[vote[pid]] > 0) counts[vote[pid]]--;  // switching vote
  vote[pid] = static_cast<int8_t>(c);
  counts[c]++;
  return true;
}

int WyrGame::votedCount() const {
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  int v = 0;
  for (int i = 0; i < n; ++i)
    if (vote[pids[i]] >= 0) v++;
  return v;
}

bool WyrGame::roundComplete() const {
  if (millis() >= deadline) return true;
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  if (n == 0) return false;
  for (int i = 0; i < n; ++i)
    if (vote[pids[i]] < 0) return false;
  return true;
}

void WyrGame::onReveal() { revealing = true; }  // poll — no scoring

void WyrGame::buildStateFor(uint8_t pid, JsonDocument& out) {
  const PackLoader::AbItem& p = prompts[roundIndex];
  out["t"] = "wyr";
  out["phase"] = revealing ? "reveal" : "vote";
  out["round"] = roundIndex;
  out["rounds"] = roundCount();
  out["a"] = p.a;
  out["b"] = p.b;
  out["myvote"] = (pid < ArcadeSession::MAX_CLIENTS) ? vote[pid] : -1;
  JsonArray c = out["counts"].to<JsonArray>();
  c.add(counts[0]);
  c.add(counts[1]);
}

void WyrGame::buildFinal(JsonDocument& out) {
  out["t"] = "wyr";
  out["phase"] = "final";
  session->fillBoard(out);
}

void WyrGame::roundLabel(char* buf, int len) const {
  snprintf(buf, len, "Prompt %d / %d", roundIndex + 1, roundCount());
}

void WyrGame::tallyLine(char* buf, int len) const {
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  snprintf(buf, len, "Voted %d/%d  (A:%u B:%u)", votedCount(), n, counts[0], counts[1]);
}
