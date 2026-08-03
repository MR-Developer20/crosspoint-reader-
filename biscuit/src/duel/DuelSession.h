#pragma once
#include <ArduinoJson.h>
#include <WebSocketsServer.h>

#include <cstddef>
#include <cstdint>

#include "PoolTable.h"

// One pool table: two seats + spectators, turn-based, single-threaded.
//
// The shooting phone runs the physics and reports the outcome; DuelSession
// relays that through PoolTable (the rules referee) and broadcasts the new
// state to everyone. No FreeRTOS tasks, no mutexes — driven from the host
// activity's loop().
class DuelSession {
 public:
  static constexpr int MAX_PARTICIPANTS = 8;  // 2 players + up to 6 spectators
  static constexpr uint8_t NO_WS = 255;
  static constexpr int WS_SLOTS = 8;

  struct Participant {
    bool active = false;
    uint8_t wsNum = NO_WS;
    char nick[24] = {0};
    int seat = -1;  // 0/1 = player, -1 = spectator
  };

  void begin(WebSocketsServer* ws) {
    wsServer = ws;
    reset();
  }
  void reset();

  // WebSocket events (from the host activity trampoline).
  bool onConnect(uint8_t wsNum);  // false → caller rejects (full)
  void onDisconnect(uint8_t wsNum);
  void onText(uint8_t wsNum, const char* payload, size_t length);

  // Host controls (device buttons).
  void hostStart();     // begin a match when two are seated
  void hostRematch();   // after gameover, re-rack (loser breaks)
  void hostReset();     // back to an empty table (frees seats)

  // Host-screen accessors.
  const PoolTable& table() const { return pool; }
  int seatedCount() const;
  int spectatorCount() const;
  const char* seatNick(int seat) const;
  int connectedSockets() const { return connectedClients; }
  int rejectedSockets() const { return rejectedClients; }
  uint32_t minHeap() const { return minHeapSeen; }
  bool consumeDirty() {
    bool d = dirty;
    dirty = false;
    return d;
  }

 private:
  WebSocketsServer* wsServer = nullptr;
  PoolTable pool;

  Participant parts[MAX_PARTICIPANTS];
  bool counted[WS_SLOTS] = {false};
  int connectedClients = 0;
  int rejectedClients = 0;
  uint32_t minHeapSeen = 0xFFFFFFFF;
  bool dirty = false;
  int breaker = 0;  // seat that breaks next rack

  int indexByWs(uint8_t wsNum) const;
  int freeIndex() const;
  int openSeat() const;

  void handleHello(int idx, const JsonDocument& msg);
  void handleSit(int idx);
  void handleStand(int idx);
  void handleShot(int idx, const JsonDocument& msg);

  void broadcastState();
  void sendState(int idx);
  void sendToast(uint8_t wsNum, const char* msg);
  void sendDoc(uint8_t wsNum, JsonDocument& d);
  static void sanitizeNick(const char* in, char* out, size_t outLen);
  static const char* groupName(PoolTable::Group g);

  char txBuf[1280];
  JsonDocument doc;
};
