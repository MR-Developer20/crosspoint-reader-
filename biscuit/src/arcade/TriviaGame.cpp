#include "TriviaGame.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

bool TriviaGame::load(const std::string& path) {
  return PackLoader::loadTrivia(path, questions, QUESTION_CAP, packName, sizeof(packName));
}

void TriviaGame::onRoundStart(int round) {
  roundIndex = round;
  revealing = false;
  for (int i = 0; i < ArcadeSession::MAX_CLIENTS; ++i) {
    choice[i] = -1;
    answered[i] = false;
    answerMs[i] = 0;
  }
  for (int i = 0; i < 4; ++i) counts[i] = 0;
  deadline = millis() + static_cast<uint32_t>(DUR_SECONDS) * 1000;
}

bool TriviaGame::onClientIntent(uint8_t pid, const JsonDocument& msg) {
  if (revealing || pid >= ArcadeSession::MAX_CLIENTS) return false;
  if (strcmp(msg["t"] | "", "answer") != 0) return false;
  if (answered[pid]) return false;
  int c = msg["c"] | -1;
  if (c < 0 || c > 3) return false;
  choice[pid] = static_cast<int8_t>(c);
  answered[pid] = true;
  answerMs[pid] = millis();
  counts[c]++;
  return true;
}

int TriviaGame::answeredCount() const {
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  int a = 0;
  for (int i = 0; i < n; ++i)
    if (answered[pids[i]]) a++;
  return a;
}

bool TriviaGame::roundComplete() const {
  if (millis() >= deadline) return true;
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  if (n == 0) return false;
  for (int i = 0; i < n; ++i)
    if (!answered[pids[i]]) return false;
  return true;  // all active answered
}

void TriviaGame::onReveal() {
  revealing = true;
  const uint8_t correct = questions[roundIndex].correct;
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  for (int i = 0; i < n; ++i) {
    uint8_t pid = pids[i];
    if (!answered[pid] || choice[pid] != static_cast<int8_t>(correct)) continue;
    long remaining = static_cast<long>(deadline) - static_cast<long>(answerMs[pid]);
    if (remaining < 0) remaining = 0;
    uint32_t speed = static_cast<uint32_t>(static_cast<long>(MAX_SPEED_BONUS) * remaining /
                                           (static_cast<long>(DUR_SECONDS) * 1000L));
    session->awardPoints(pid, BASE_POINTS + speed);
  }
}

void TriviaGame::buildStateFor(uint8_t pid, JsonDocument& out) {
  const PackLoader::TriviaQ& q = questions[roundIndex];
  out["t"] = "trivia";
  out["phase"] = revealing ? "reveal" : "question";
  out["i"] = roundIndex;
  out["total"] = roundCount();
  out["q"] = q.q;
  JsonArray o = out["o"].to<JsonArray>();
  for (int k = 0; k < 4; ++k) o.add(q.opt[k]);
  out["dur"] = DUR_SECONDS;
  out["deadline"] = deadline;
  out["mine"] = (pid < ArcadeSession::MAX_CLIENTS) ? choice[pid] : -1;
  JsonArray c = out["counts"].to<JsonArray>();
  for (int k = 0; k < 4; ++k) c.add(counts[k]);
  if (revealing) out["correct"] = q.correct;
  session->fillScores(out);
}

void TriviaGame::buildFinal(JsonDocument& out) {
  out["t"] = "trivia";
  out["phase"] = "final";
  session->fillBoard(out);
}

void TriviaGame::roundLabel(char* buf, int len) const {
  snprintf(buf, len, "Q %d / %d", roundIndex + 1, roundCount());
}

void TriviaGame::tallyLine(char* buf, int len) const {
  uint8_t pids[ArcadeSession::MAX_CLIENTS];
  int n = session->activePids(pids, ArcadeSession::MAX_CLIENTS);
  snprintf(buf, len, "Answered %d/%d", answeredCount(), n);
}
