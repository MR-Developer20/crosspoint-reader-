#include "ArcadeSession.h"

#include <Arduino.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "PartyGame.h"

ArcadeSession::~ArcadeSession() = default;

void ArcadeSession::reset() {
  for (auto& p : players) p = Player{};
  for (bool& c : counted) c = false;
  phase = Phase::LOBBY;
  connectedClients = 0;
  rejectedClients = 0;
  minHeapSeen = 0xFFFFFFFF;
  msgs = 0;
  countdownN = 0;
  curRound = 0;
  dirty = true;
  game.reset();
}

// ---------------------------------------------------------------- slots

int ArcadeSession::slotByWs(uint8_t wsNum) const {
  for (int i = 0; i < MAX_CLIENTS; ++i)
    if (players[i].active && players[i].wsNum == wsNum) return i;
  return -1;
}

int ArcadeSession::freeSlot() const {
  for (int i = 0; i < MAX_CLIENTS; ++i)
    if (!players[i].active) return i;
  return -1;
}

int ArcadeSession::playerCount() const {
  int n = 0;
  for (const auto& p : players)
    if (p.active) ++n;
  return n;
}

int ArcadeSession::readyCount() const {
  int n = 0;
  for (const auto& p : players)
    if (p.active && p.ready) ++n;
  return n;
}

// ---------------------------------------------------------------- ws events

bool ArcadeSession::onConnect(uint8_t wsNum) {
  if (connectedClients >= MAX_CLIENTS) {
    // Accept-then-reject: prove the "lobby full" path. Caller disconnects.
    rejectedClients++;
    sendToast(wsNum, "Lobby full");
    LOG_DBG("ARC", "Reject socket %u (full, %d connected)", wsNum, connectedClients);
    return false;
  }
  if (wsNum < WS_SLOTS && !counted[wsNum]) {
    counted[wsNum] = true;
    connectedClients++;
  }
  dirty = true;
  return true;
}

void ArcadeSession::onDisconnect(uint8_t wsNum) {
  if (wsNum < WS_SLOTS && counted[wsNum]) {
    counted[wsNum] = false;
    if (connectedClients > 0) connectedClients--;
  }
  int slot = slotByWs(wsNum);
  if (slot >= 0) {
    players[slot] = Player{};
    broadcastLobby();
    // A drop below the ready threshold aborts a pending countdown.
    if (phase == Phase::COUNTDOWN) cancelCountdown();
  }
  dirty = true;
}

void ArcadeSession::onText(uint8_t wsNum, const char* payload, size_t length) {
  msgs++;
  uint32_t heap = ESP.getFreeHeap();
  if (heap < minHeapSeen) minHeapSeen = heap;

  doc.clear();
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    LOG_DBG("ARC", "Bad JSON from %u: %s", wsNum, err.c_str());
    return;
  }
  const char* t = doc["t"] | "";

  if (strcmp(t, "ping") == 0) {
    // M1 spike keepalive: echo with live heap.
    long n = doc["n"] | 0;
    JsonDocument out;
    out["t"] = "pong";
    out["n"] = n;
    out["heap"] = ESP.getFreeHeap();
    sendDoc(wsNum, out);
    return;
  }
  if (strcmp(t, "pong") == 0) return;

  if (strcmp(t, "hello") == 0) {
    int slot = slotByWs(wsNum);
    if (slot < 0) slot = freeSlot();
    if (slot < 0) {
      sendToast(wsNum, "Lobby full");
      return;
    }
    players[slot].wsNum = wsNum;  // map socket → slot before any reply
    handleHello(slot, doc);       // nick is read before any member-doc broadcast
    return;
  }

  int slot = slotByWs(wsNum);
  if (slot < 0) return;  // not joined yet — ignore game intents

  if (strcmp(t, "ready") == 0) {
    handleReady(slot, doc);  // ready flag read before broadcast clears the doc
    return;
  }

  // answer / guess / tap → active game (fields consumed before broadcast)
  routeIntent(slot, doc);
}

// ---------------------------------------------------------------- lobby

void ArcadeSession::handleHello(int slot, const JsonDocument& msg) {
  Player& p = players[slot];
  p.active = true;  // wsNum already mapped by caller
  const char* nick = msg["nick"] | "Player";
  sanitizeNick(nick, p.nick, sizeof(p.nick));
  p.ready = false;
  // score persists across a game switch reset only via setGame(); a fresh join
  // starts at 0.
  if (phase == Phase::LOBBY) p.score = 0;
  sendWelcome(slot);
  broadcastLobby();
  // A mid-game (re)join gets the live round state immediately instead of being
  // stranded on the lobby view until the next broadcast.
  if (game && (phase == Phase::ROUND || phase == Phase::REVEAL)) {
    doc.clear();
    game->buildStateFor(static_cast<uint8_t>(slot), doc);
    sendDoc(players[slot].wsNum, doc);
  }
  dirty = true;
}

void ArcadeSession::handleReady(int slot, const JsonDocument& msg) {
  bool r = msg["ready"] | false;
  players[slot].ready = r;
  broadcastLobby();
  dirty = true;
  if (phase == Phase::LOBBY)
    startCountdownIfReady();
  else if (phase == Phase::COUNTDOWN && !r)
    cancelCountdown();
}

void ArcadeSession::routeIntent(int slot, const JsonDocument& msg) {
  if (phase != Phase::ROUND || !game) return;
  if (game->onClientIntent(static_cast<uint8_t>(slot), msg)) {
    broadcastGameState();
    dirty = true;  // host "Answered k/n" advances on real progress only
  }
}

// ---------------------------------------------------------------- flow

void ArcadeSession::startCountdownIfReady() {
  if (phase != Phase::LOBBY || !game) return;
  int active = playerCount();
  if (active >= 2 && readyCount() == active) {
    phase = Phase::COUNTDOWN;
    countdownN = 5;
    JsonDocument out;
    out["t"] = "countdown";
    out["n"] = countdownN;
    for (const auto& p : players)
      if (p.active) sendDoc(p.wsNum, out);
    nextCountdownAt = millis() + 1000;
    // Countdown ticks intentionally do NOT set dirty — host stays static.
  }
}

void ArcadeSession::cancelCountdown() {
  phase = Phase::LOBBY;
  countdownN = 0;
  broadcastLobby();
  dirty = true;
}

void ArcadeSession::startGame() {
  if (!game) {
    phase = Phase::LOBBY;
    return;
  }
  curRound = 0;
  game->onRoundStart(curRound);
  phase = Phase::ROUND;
  broadcastGameState();
  dirty = true;
}

void ArcadeSession::advanceRound() {
  if (!game) return;
  curRound++;
  if (curRound >= game->roundCount()) {
    phase = Phase::FINAL;
    JsonDocument out;
    game->buildFinal(out);
    for (const auto& p : players)
      if (p.active) sendDoc(p.wsNum, out);
    dirty = true;
    return;
  }
  game->onRoundStart(curRound);
  phase = Phase::ROUND;
  broadcastGameState();
  dirty = true;
}

void ArcadeSession::tick() {
  switch (phase) {
    case Phase::COUNTDOWN: {
      if (millis() >= nextCountdownAt) {
        countdownN--;
        if (countdownN > 0) {
          JsonDocument out;
          out["t"] = "countdown";
          out["n"] = countdownN;
          for (const auto& p : players)
            if (p.active) sendDoc(p.wsNum, out);
          nextCountdownAt += 1000;
        } else {
          startGame();
        }
      }
      break;
    }
    case Phase::ROUND: {
      if (!game) break;
      if (game->onTick()) broadcastGameState();
      if (game->roundComplete()) {
        game->onReveal();
        phase = Phase::REVEAL;
        broadcastGameState();
        revealUntil = millis() + REVEAL_AUTO_MS;
        dirty = true;
      }
      break;
    }
    case Phase::REVEAL: {
      if (millis() >= revealUntil) advanceRound();
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------- host control

void ArcadeSession::setGame(std::unique_ptr<PartyGame> g) {
  game = std::move(g);
  if (game) game->setSession(this);
  curRound = 0;
  phase = Phase::LOBBY;
  for (auto& p : players) {
    if (p.active) {
      p.score = 0;
      p.ready = false;
    }
  }
  broadcastLobby();
  dirty = true;
}

void ArcadeSession::hostReveal() {
  if (phase != Phase::ROUND || !game) return;
  game->onReveal();
  phase = Phase::REVEAL;
  broadcastGameState();
  revealUntil = millis() + REVEAL_AUTO_MS;
  dirty = true;
}

void ArcadeSession::hostAdvance() {
  if (phase == Phase::REVEAL) {
    advanceRound();
  } else if (phase == Phase::FINAL) {
    phase = Phase::LOBBY;
    for (auto& p : players)
      if (p.active) p.ready = false;
    broadcastLobby();
    dirty = true;
  }
}

// ---------------------------------------------------------------- game services

void ArcadeSession::awardPoints(uint8_t pid, uint32_t pts) {
  if (pid < MAX_CLIENTS && players[pid].active) players[pid].score += pts;
}

const ArcadeSession::Player* ArcadeSession::playerByPid(uint8_t pid) const {
  if (pid < MAX_CLIENTS && players[pid].active) return &players[pid];
  return nullptr;
}

const char* ArcadeSession::nickByPid(uint8_t pid) const {
  const Player* p = playerByPid(pid);
  return p ? p->nick : "";
}

int ArcadeSession::activePids(uint8_t* out, int maxOut) const {
  int n = 0;
  for (int i = 0; i < MAX_CLIENTS && n < maxOut; ++i)
    if (players[i].active) out[n++] = static_cast<uint8_t>(i);
  return n;
}

void ArcadeSession::fillScores(JsonDocument& out, const char* key) const {
  // Collect active slots, sort by score desc (stable by slot for ties).
  int idx[MAX_CLIENTS];
  int n = 0;
  for (int i = 0; i < MAX_CLIENTS; ++i)
    if (players[i].active) idx[n++] = i;
  std::sort(idx, idx + n, [this](int a, int b) { return players[a].score > players[b].score; });
  JsonArray arr = out[key].to<JsonArray>();
  for (int k = 0; k < n; ++k) {
    JsonObject o = arr.add<JsonObject>();
    o["pid"] = idx[k];
    o["nick"] = players[idx[k]].nick;
    o["score"] = players[idx[k]].score;
  }
}

void ArcadeSession::fillBoard(JsonDocument& out) const { fillScores(out, "board"); }

// ---------------------------------------------------------------- broadcast

void ArcadeSession::broadcastGameState() {
  if (!game) return;
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (!players[i].active) continue;
    doc.clear();
    game->buildStateFor(static_cast<uint8_t>(i), doc);
    sendDoc(players[i].wsNum, doc);
  }
}

void ArcadeSession::broadcastLobby() {
  const char* gid = game ? game->id() : "none";
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (!players[i].active) continue;
    doc.clear();
    doc["t"] = "lobby";
    doc["game"] = gid;
    doc["me"] = i;
    JsonArray arr = doc["players"].to<JsonArray>();
    for (int j = 0; j < MAX_CLIENTS; ++j) {
      if (!players[j].active) continue;
      JsonObject o = arr.add<JsonObject>();
      o["pid"] = j;
      o["nick"] = players[j].nick;
      o["score"] = players[j].score;
      o["ready"] = players[j].ready;
    }
    sendDoc(players[i].wsNum, doc);
  }
}

void ArcadeSession::sendWelcome(int slot) {
  JsonDocument out;
  out["t"] = "welcome";
  out["pid"] = slot;
  out["nick"] = players[slot].nick;
  sendDoc(players[slot].wsNum, out);
}

void ArcadeSession::sendToast(uint8_t wsNum, const char* msg) {
  JsonDocument out;
  out["t"] = "toast";
  out["msg"] = msg;
  sendDoc(wsNum, out);
}

void ArcadeSession::sendDoc(uint8_t wsNum, JsonDocument& d) {
  if (!wsServer || wsNum == NO_WS) return;
  size_t len = serializeJson(d, txBuf, sizeof(txBuf));
  if (len == 0 || len >= sizeof(txBuf)) {
    LOG_ERR("ARC", "TX overflow (%u bytes)", (unsigned)len);
    return;
  }
  wsServer->sendTXT(wsNum, txBuf, len);
}

// ---------------------------------------------------------------- util

void ArcadeSession::sanitizeNick(const char* in, char* out, size_t outLen) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < outLen; ++i) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c >= 0x20 && c != 0x7F) out[o++] = static_cast<char>(c);  // strip control chars
  }
  if (o == 0 && outLen > 1) {  // empty after sanitize
    const char* fb = "Player";
    for (size_t i = 0; fb[i] && o + 1 < outLen; ++i) out[o++] = fb[i];
  }
  out[o] = '\0';
}
