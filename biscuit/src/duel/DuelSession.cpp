#include "DuelSession.h"

#include <Arduino.h>
#include <Logging.h>

#include <cstring>

void DuelSession::reset() {
  for (auto& p : parts) p = Participant{};
  for (bool& c : counted) c = false;
  connectedClients = 0;
  rejectedClients = 0;
  minHeapSeen = 0xFFFFFFFF;
  breaker = 0;
  pool = PoolTable{};
  dirty = true;
}

int DuelSession::indexByWs(uint8_t wsNum) const {
  for (int i = 0; i < MAX_PARTICIPANTS; ++i)
    if (parts[i].active && parts[i].wsNum == wsNum) return i;
  return -1;
}

int DuelSession::freeIndex() const {
  for (int i = 0; i < MAX_PARTICIPANTS; ++i)
    if (!parts[i].active) return i;
  return -1;
}

int DuelSession::openSeat() const {
  bool taken[2] = {false, false};
  for (const auto& p : parts)
    if (p.active && (p.seat == 0 || p.seat == 1)) taken[p.seat] = true;
  if (!taken[0]) return 0;
  if (!taken[1]) return 1;
  return -1;
}

int DuelSession::seatedCount() const {
  int n = 0;
  for (const auto& p : parts)
    if (p.active && p.seat >= 0) ++n;
  return n;
}

int DuelSession::spectatorCount() const {
  int n = 0;
  for (const auto& p : parts)
    if (p.active && p.seat < 0) ++n;
  return n;
}

const char* DuelSession::seatNick(int seat) const {
  for (const auto& p : parts)
    if (p.active && p.seat == seat) return p.nick;
  return "";
}

const char* DuelSession::groupName(PoolTable::Group g) {
  switch (g) {
    case PoolTable::Group::SOLIDS: return "solids";
    case PoolTable::Group::STRIPES: return "stripes";
    default: return "open";
  }
}

// ---------------------------------------------------------------- ws events

bool DuelSession::onConnect(uint8_t wsNum) {
  if (connectedClients >= MAX_PARTICIPANTS) {
    rejectedClients++;
    sendToast(wsNum, "Table full");
    return false;
  }
  if (wsNum < WS_SLOTS && !counted[wsNum]) {
    counted[wsNum] = true;
    connectedClients++;
  }
  dirty = true;
  return true;
}

void DuelSession::onDisconnect(uint8_t wsNum) {
  if (wsNum < WS_SLOTS && counted[wsNum]) {
    counted[wsNum] = false;
    if (connectedClients > 0) connectedClients--;
  }
  int idx = indexByWs(wsNum);
  if (idx >= 0) {
    bool wasSeated = parts[idx].seat >= 0;
    parts[idx] = Participant{};
    // A seated player leaving mid-match ends it.
    if (wasSeated && pool.phase == PoolTable::Phase::PLAYING) {
      pool.phase = PoolTable::Phase::WAITING;
      strncpy(pool.message, "Opponent left", sizeof(pool.message) - 1);
    }
    broadcastState();
  }
  dirty = true;
}

void DuelSession::onText(uint8_t wsNum, const char* payload, size_t length) {
  uint32_t heap = ESP.getFreeHeap();
  if (heap < minHeapSeen) minHeapSeen = heap;

  doc.clear();
  if (deserializeJson(doc, payload, length)) return;
  const char* t = doc["t"] | "";

  if (strcmp(t, "ping") == 0) {
    JsonDocument out;
    out["t"] = "pong";
    out["n"] = doc["n"] | 0;
    out["heap"] = ESP.getFreeHeap();
    sendDoc(wsNum, out);
    return;
  }
  if (strcmp(t, "pong") == 0) return;

  if (strcmp(t, "hello") == 0) {
    int idx = indexByWs(wsNum);
    if (idx < 0) idx = freeIndex();
    if (idx < 0) {
      sendToast(wsNum, "Table full");
      return;
    }
    parts[idx].wsNum = wsNum;
    handleHello(idx, doc);
    return;
  }

  int idx = indexByWs(wsNum);
  if (idx < 0) return;

  if (strcmp(t, "sit") == 0) {
    handleSit(idx);
  } else if (strcmp(t, "stand") == 0) {
    handleStand(idx);
  } else if (strcmp(t, "start") == 0) {
    hostStart();
  } else if (strcmp(t, "rematch") == 0) {
    hostRematch();
  } else if (strcmp(t, "shot") == 0) {
    handleShot(idx, doc);
  }
}

void DuelSession::handleHello(int idx, const JsonDocument& msg) {
  Participant& p = parts[idx];
  p.active = true;
  sanitizeNick(msg["nick"] | "Player", p.nick, sizeof(p.nick));
  // Auto-seat the first two joiners; the rest spectate.
  if (p.seat < 0 && pool.phase != PoolTable::Phase::PLAYING) {
    int s = openSeat();
    if (s >= 0) p.seat = s;
  }
  broadcastState();
  dirty = true;
}

void DuelSession::handleSit(int idx) {
  Participant& p = parts[idx];
  if (p.seat >= 0) return;
  if (pool.phase == PoolTable::Phase::PLAYING) return;  // no sitting into a live game
  int s = openSeat();
  if (s < 0) return;
  p.seat = s;
  broadcastState();
  dirty = true;
}

void DuelSession::handleStand(int idx) {
  Participant& p = parts[idx];
  if (p.seat < 0) return;
  if (pool.phase == PoolTable::Phase::PLAYING) return;  // can't stand mid-match
  p.seat = -1;
  broadcastState();
  dirty = true;
}

void DuelSession::handleShot(int idx, const JsonDocument& msg) {
  Participant& p = parts[idx];
  if (p.seat < 0 || pool.phase != PoolTable::Phase::PLAYING) return;
  if (p.seat != pool.turn) return;  // not your turn

  PoolTable::Ball nb[PoolTable::NUM_BALLS];
  // Start from the current positions so any ball the phone omits stays put.
  for (int i = 0; i < PoolTable::NUM_BALLS; ++i) nb[i] = pool.balls[i];
  JsonArrayConst arr = msg["balls"].as<JsonArrayConst>();
  for (JsonVariantConst o : arr) {
    int id = o["id"] | -1;
    if (id < 0 || id >= PoolTable::NUM_BALLS) continue;
    nb[id].x = o["x"] | nb[id].x;
    nb[id].y = o["y"] | nb[id].y;
    nb[id].pocketed = o["pk"] | false;
  }
  int firstHit = msg["firstHit"] | -1;
  bool rail = msg["rail"] | false;
  float vx = msg["vx"] | 0.0f;
  float vy = msg["vy"] | 0.0f;

  pool.applyShot(p.seat, nb, firstHit, rail, vx, vy);

  // Winner breaks-loser next rack convention: loser of this game breaks.
  if (pool.phase == PoolTable::Phase::GAMEOVER && pool.winner >= 0) breaker = 1 - pool.winner;

  broadcastState();
  dirty = true;
}

// ---------------------------------------------------------------- host control

void DuelSession::hostStart() {
  if (pool.phase == PoolTable::Phase::PLAYING) return;
  if (seatedCount() < 2) return;
  pool.rack(breaker);
  broadcastState();
  dirty = true;
}

void DuelSession::hostRematch() {
  if (pool.phase != PoolTable::Phase::GAMEOVER) return;
  if (seatedCount() < 2) {
    pool.phase = PoolTable::Phase::WAITING;
  } else {
    pool.rack(breaker);
  }
  broadcastState();
  dirty = true;
}

void DuelSession::hostReset() {
  pool = PoolTable{};
  for (auto& p : parts)
    if (p.active) p.seat = -1;
  broadcastState();
  dirty = true;
}

// ---------------------------------------------------------------- broadcast

void DuelSession::broadcastState() {
  for (int i = 0; i < MAX_PARTICIPANTS; ++i)
    if (parts[i].active) sendState(i);
}

void DuelSession::sendState(int idx) {
  const Participant& me = parts[idx];
  doc.clear();
  doc["t"] = "state";
  const char* ph = pool.phase == PoolTable::Phase::WAITING
                       ? "waiting"
                       : (pool.phase == PoolTable::Phase::PLAYING ? "playing" : "gameover");
  doc["phase"] = ph;
  doc["turn"] = pool.turn;
  doc["ballInHand"] = pool.ballInHand;
  doc["onBreak"] = pool.onBreak;
  doc["winner"] = pool.winner;
  doc["message"] = pool.message;
  doc["you"] = me.seat;
  doc["yourTurn"] = (me.seat == pool.turn && pool.phase == PoolTable::Phase::PLAYING);
  doc["spectators"] = spectatorCount();
  doc["W"] = PoolTable::W;
  doc["H"] = PoolTable::H;

  JsonArray players = doc["players"].to<JsonArray>();
  for (int s = 0; s < 2; ++s) {
    JsonObject o = players.add<JsonObject>();
    o["seat"] = s;
    o["nick"] = seatNick(s);
    o["group"] = groupName(pool.groups[s]);
    o["cleared"] = pool.clearedGroup(s);
  }

  JsonArray ba = doc["balls"].to<JsonArray>();
  for (int id = 0; id < PoolTable::NUM_BALLS; ++id) {
    JsonObject o = ba.add<JsonObject>();
    o["id"] = id;
    o["x"] = pool.balls[id].x;
    o["y"] = pool.balls[id].y;
    o["pk"] = pool.balls[id].pocketed;
  }

  // Replay hint (opponent/spectator re-simulate then snap).
  doc["shotSeq"] = pool.shotSeq;
  doc["lastBy"] = pool.lastBy;
  doc["lcx"] = pool.lastCueX;
  doc["lcy"] = pool.lastCueY;
  doc["lvx"] = pool.lastVX;
  doc["lvy"] = pool.lastVY;

  sendDoc(me.wsNum, doc);
}

void DuelSession::sendToast(uint8_t wsNum, const char* msg) {
  JsonDocument out;
  out["t"] = "toast";
  out["msg"] = msg;
  sendDoc(wsNum, out);
}

void DuelSession::sendDoc(uint8_t wsNum, JsonDocument& d) {
  if (!wsServer || wsNum == NO_WS) return;
  size_t len = serializeJson(d, txBuf, sizeof(txBuf));
  if (len == 0 || len >= sizeof(txBuf)) {
    LOG_ERR("DUEL", "TX overflow (%u)", (unsigned)len);
    return;
  }
  wsServer->sendTXT(wsNum, txBuf, len);
}

void DuelSession::sanitizeNick(const char* in, char* out, size_t outLen) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < outLen; ++i) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c >= 0x20 && c != 0x7F) out[o++] = static_cast<char>(c);
  }
  if (o == 0 && outLen > 1) {
    const char* fb = "Player";
    for (size_t i = 0; fb[i] && o + 1 < outLen; ++i) out[o++] = fb[i];
  }
  out[o] = '\0';
}
