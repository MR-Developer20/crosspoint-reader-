#include "GuessColorGame.h"

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <cstring>

void GuessColorGame::hex(uint8_t r, uint8_t g, uint8_t b, char* out) {
  snprintf(out, 8, "#%02X%02X%02X", r, g, b);
}

void GuessColorGame::onRoundStart(int round) {
  roundIndex = round;
  revealing = false;
  winner = -1;
  tr = static_cast<uint8_t>(random(0, 256));
  tg = static_cast<uint8_t>(random(0, 256));
  tb = static_cast<uint8_t>(random(0, 256));
  for (int i = 0; i < ArcadeSession::MAX_CLIENTS; ++i) {
    submitted[i] = false;
    gr[i] = gg[i] = gb[i] = 0;
    submitMs[i] = 0;
    dist[i] = 0;
    points[i] = 0;
  }
  startMs = millis();
  deadline = startMs + ROUND_TIMEOUT_MS;
}

static uint8_t clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v)); }

bool GuessColorGame::onClientIntent(uint8_t pid, const JsonDocument& msg) {
  if (revealing || pid >= ArcadeSession::MAX_CLIENTS) return false;
  if (strcmp(msg["t"] | "", "guess") != 0) return false;
  // Route by fields present: gc owns guess{r,g,b}; scramble owns guess{text}.
  if (!msg["r"].is<int>() || !msg["g"].is<int>() || !msg["b"].is<int>()) return false;
  if (submitted[pid]) return false;
  gr[pid] = clamp255(msg["r"] | 0);
  gg[pid] = clamp255(msg["g"] | 0);
  gb[pid] = clamp255(msg["b"] | 0);
  submitMs[pid] = millis();
  submitted[pid] = true;
  return true;
}

int GuessColorGame::submittedCount() const {
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  int s = 0;
  for (int i = 0; i < n; ++i)
    if (submitted[pids[i]]) s++;
  return s;
}

bool GuessColorGame::roundComplete() const {
  if (millis() >= deadline) return true;
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  if (n == 0) return false;
  for (int i = 0; i < n; ++i)
    if (!submitted[pids[i]]) return false;
  return true;
}

void GuessColorGame::onReveal() {
  revealing = true;
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  int bestPts = -1;
  uint32_t bestMs = 0xFFFFFFFF;
  for (int i = 0; i < n; ++i) {
    uint8_t pid = pids[i];
    if (!submitted[pid]) {
      dist[pid] = -1;
      points[pid] = 0;
      continue;
    }
    int dr = static_cast<int>(gr[pid]) - static_cast<int>(tr);
    int dg = static_cast<int>(gg[pid]) - static_cast<int>(tg);
    int db = static_cast<int>(gb[pid]) - static_cast<int>(tb);
    float d = sqrtf(static_cast<float>(dr * dr + dg * dg + db * db));
    dist[pid] = static_cast<int>(lroundf(d));

    int closeness = static_cast<int>(lroundf(200.0f * (1.0f - d / MAX_DIST)));
    if (closeness < 0) closeness = 0;
    uint32_t subMs = submitMs[pid] - startMs;
    int speed = static_cast<int>(lroundf(100.0f * (1.0f - static_cast<float>(subMs) / SPEED_WINDOW_MS)));
    if (speed < 0) speed = 0;
    int pts = closeness + speed;
    points[pid] = pts;
    session->awardPoints(pid, static_cast<uint32_t>(pts));

    // Round winner = highest points; ties broken by faster submit.
    if (pts > bestPts || (pts == bestPts && subMs < bestMs)) {
      bestPts = pts;
      bestMs = subMs;
      winner = pid;
    }
  }
}

void GuessColorGame::buildStateFor(uint8_t pid, JsonDocument& out) {
  char c[8];
  hex(tr, tg, tb, c);
  out["t"] = "gc";
  out["phase"] = revealing ? "reveal" : "play";
  out["round"] = roundIndex;
  out["rounds"] = ROUNDS;
  out["color"] = c;  // swatch shown to phones; numbers hidden until reveal
  out["submitted"] = (pid < ArcadeSession::MAX_CLIENTS) ? submitted[pid] : false;
  if (revealing) {
    out["r"] = tr;
    out["g"] = tg;
    out["b"] = tb;
    if (pid < ArcadeSession::MAX_CLIENTS && submitted[pid]) {
      char yc[8];
      hex(gr[pid], gg[pid], gb[pid], yc);
      JsonObject y = out["your"].to<JsonObject>();
      y["r"] = gr[pid];
      y["g"] = gg[pid];
      y["b"] = gb[pid];
      y["color"] = yc;
      y["dist"] = dist[pid];
      y["points"] = points[pid];
    } else {
      out["your"] = nullptr;
    }
    out["winner"] = (winner >= 0) ? session->nickByPid(static_cast<uint8_t>(winner)) : nullptr;
    out["iwon"] = (winner >= 0 && static_cast<int>(pid) == winner);
  }
  session->fillScores(out);
}

void GuessColorGame::buildFinal(JsonDocument& out) {
  out["t"] = "gc";
  out["phase"] = "final";
  session->fillBoard(out);
}

void GuessColorGame::roundLabel(char* buf, int len) const {
  snprintf(buf, len, "Round %d / %d", roundIndex + 1, ROUNDS);
}

void GuessColorGame::tallyLine(char* buf, int len) const {
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  snprintf(buf, len, "Submitted %d/%d", submittedCount(), n);
}
