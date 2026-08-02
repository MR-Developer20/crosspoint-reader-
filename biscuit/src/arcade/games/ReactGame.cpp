#include "ReactGame.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

void ReactGame::onRoundStart(int round) {
  roundIndex = round;
  revealing = false;
  go = false;
  winner = -1;
  winnerMs = 0;
  for (int i = 0; i < ArcadeSession::MAX_CLIENTS; ++i) {
    dq[i] = false;
    tapped[i] = false;
  }
  goAt = millis() + static_cast<uint32_t>(random(GO_MIN_MS, GO_MAX_MS + 1));
}

bool ReactGame::onTick() {
  if (!go && !revealing && millis() >= goAt) {
    go = true;
    goMs = millis();
    return true;  // rebroadcast: light flips to "go"
  }
  return false;
}

bool ReactGame::onClientIntent(uint8_t pid, const JsonDocument& msg) {
  if (revealing || pid >= ArcadeSession::MAX_CLIENTS) return false;
  if (strcmp(msg["t"] | "", "tap") != 0) return false;
  if (dq[pid] || tapped[pid]) return false;  // already resolved this round
  if (!go) {
    dq[pid] = true;  // jumped the gun
    return true;
  }
  tapped[pid] = true;
  if (winner < 0) {
    winner = pid;
    winnerMs = millis() - goMs;
    session->awardPoints(pid, WINNER_POINTS);
  }
  return true;
}

bool ReactGame::allResolved() const {
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  if (n == 0) return false;
  for (int i = 0; i < n; ++i)
    if (!dq[pids[i]] && !tapped[pids[i]]) return false;
  return true;
}

bool ReactGame::roundComplete() const {
  if (winner >= 0) return true;
  if (go && millis() - goMs > GO_WINDOW_MS) return true;  // nobody reacted in time
  if (allResolved()) return true;                          // everyone DQ'd
  return false;
}

void ReactGame::onReveal() { revealing = true; }

void ReactGame::buildStateFor(uint8_t pid, JsonDocument& out) {
  out["t"] = "react";
  out["phase"] = revealing ? "reveal" : "armed";
  out["round"] = roundIndex;
  out["rounds"] = ROUNDS;
  out["light"] = go ? "go" : "wait";
  JsonArray dqa = out["dq"].to<JsonArray>();
  JsonArray ta = out["tapped"].to<JsonArray>();
  for (int i = 0; i < ArcadeSession::MAX_CLIENTS; ++i) {
    if (!session->slotActive(i)) continue;
    if (dq[i]) dqa.add(session->nickByPid(i));
    if (tapped[i]) ta.add(session->nickByPid(i));
  }
  if (revealing) {
    if (winner >= 0) {
      out["winner"] = session->nickByPid(static_cast<uint8_t>(winner));
      out["ms"] = winnerMs;
    } else {
      out["winner"] = nullptr;
    }
    out["iwon"] = (winner >= 0 && static_cast<int>(pid) == winner);
  }
  session->fillScores(out);
}

void ReactGame::buildFinal(JsonDocument& out) {
  out["t"] = "react";
  out["phase"] = "final";
  session->fillBoard(out);
}

void ReactGame::roundLabel(char* buf, int len) const {
  snprintf(buf, len, "Round %d / %d", roundIndex + 1, ROUNDS);
}

void ReactGame::tallyLine(char* buf, int len) const {
  if (revealing && winner >= 0)
    snprintf(buf, len, "Winner: %s (%lums)", session->nickByPid((uint8_t)winner), (unsigned long)winnerMs);
  else
    snprintf(buf, len, "%s", go ? "GO!" : "Wait for it...");
}
