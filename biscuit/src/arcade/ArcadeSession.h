#pragma once
#include <ArduinoJson.h>
#include <WebSocketsServer.h>

#include <cstddef>
#include <cstdint>
#include <memory>

class PartyGame;

// Game-agnostic roster + lobby + countdown + phase machine + broadcast.
//
// This is the piece every game reuses (M2 built it for Trivia; M3's four
// games plug straight in). It knows nothing about any specific game — it
// drives the active PartyGame through the abstract interface and relays
// per-client JSON over the WebSocket.
//
// Single-threaded: every method runs on ArcadeHostActivity::loop(). No tasks,
// no mutexes. Time advances only through tick().
class ArcadeSession {
 public:
  // Hard client ceiling. The links2004 server compile-time cap
  // (WEBSOCKETS_SERVER_CLIENT_MAX in platformio.ini) is set one higher so the
  // MAX_CLIENTS+1 socket can be accepted and then cleanly rejected ("lobby full").
  static constexpr int MAX_CLIENTS = 6;
  static constexpr uint8_t NO_WS = 255;
  // links2004 socket-slot range (WEBSOCKETS_SERVER_CLIENT_MAX). Kept > MAX_CLIENTS
  // so the overflow socket can be accepted then rejected.
  static constexpr int WS_SLOTS = 8;

  enum class Phase { LOBBY, COUNTDOWN, ROUND, REVEAL, FINAL };

  struct Player {
    bool active = false;
    uint8_t wsNum = NO_WS;
    char nick[24] = {0};
    uint32_t score = 0;
    bool ready = false;
  };

  ArcadeSession() = default;
  ~ArcadeSession();  // defined in .cpp where PartyGame is complete (unique_ptr member)

  void begin(WebSocketsServer* ws) { wsServer = ws; reset(); }
  void reset();

  // --- WebSocket event entry points (called from the host activity trampoline) ---
  // Returns true if the connection was accepted, false if rejected (lobby full).
  bool onConnect(uint8_t wsNum);
  void onDisconnect(uint8_t wsNum);
  void onText(uint8_t wsNum, const char* payload, size_t length);

  // --- Per-loop advance (countdown ticks, round deadlines, react go-signal) ---
  void tick();

  // --- Host control ---
  // Install the game the host picked. Resets scores and returns to lobby.
  void setGame(std::unique_ptr<PartyGame> game);
  void hostAdvance();  // host pressed "Next" during REVEAL, or "Play again" at FINAL
  void hostReveal();   // host forced the reveal early during a ROUND
  bool hasGame() const { return game != nullptr; }

  // --- Host-screen accessors (near-static render; no allocation) ---
  Phase getPhase() const { return phase; }
  PartyGame* activeGame() const { return game.get(); }
  int playerCount() const;         // active players
  int readyCount() const;          // active AND ready
  const Player& playerAt(int i) const { return players[i]; }  // raw slot (may be inactive)
  bool slotActive(int i) const { return players[i].active; }
  int connectedSockets() const { return connectedClients; }
  int rejectedSockets() const { return rejectedClients; }
  uint32_t minHeap() const { return minHeapSeen; }
  uint32_t msgCount() const { return msgs; }

  // Host redraw gate: set on any roster/phase change, NOT on countdown ticks
  // (keeps the e-ink screen near-static — full refresh on events only).
  bool consumeDirty() {
    bool d = dirty;
    dirty = false;
    return d;
  }

  // --- Services games call (via the PartyGame::session pointer) ---
  void awardPoints(uint8_t pid, uint32_t pts);
  const Player* playerByPid(uint8_t pid) const;    // pid == slot index
  const char* nickByPid(uint8_t pid) const;
  int activePids(uint8_t* out, int maxOut) const;  // fills active slot indices
  void fillScores(JsonDocument& out, const char* key = "scores") const;  // sorted desc
  void fillBoard(JsonDocument& out) const;                               // podium, sorted desc

  // Broadcast the active game's per-client state to every active player.
  void broadcastGameState();

 private:
  WebSocketsServer* wsServer = nullptr;
  std::unique_ptr<PartyGame> game;
  Phase phase = Phase::LOBBY;

  Player players[MAX_CLIENTS];
  int connectedClients = 0;  // raw sockets (joined or not)
  int rejectedClients = 0;
  uint32_t minHeapSeen = 0xFFFFFFFF;
  uint32_t msgs = 0;
  bool dirty = false;

  bool counted[WS_SLOTS] = {false};  // accepted (non-rejected) open sockets
  int curRound = 0;

  // Countdown / round / reveal timing (all millis()-based, checked in tick())
  int countdownN = 0;
  uint32_t nextCountdownAt = 0;
  uint32_t revealUntil = 0;
  static constexpr uint32_t REVEAL_AUTO_MS = 6000;

  int slotByWs(uint8_t wsNum) const;
  int freeSlot() const;

  void handleHello(int slot, const JsonDocument& msg);
  void handleReady(int slot, const JsonDocument& msg);
  void routeIntent(int slot, const JsonDocument& msg);

  void startCountdownIfReady();
  void cancelCountdown();
  void startGame();
  void advanceRound();

  // Outgoing helpers (fixed buffer, no heap churn per message)
  void sendDoc(uint8_t wsNum, JsonDocument& doc);
  void broadcastLobby();
  void sendWelcome(int slot);
  void sendToast(uint8_t wsNum, const char* msg);

  static void sanitizeNick(const char* in, char* out, size_t outLen);

  // Sized for the largest message (trivia question + 4 options + 6-player
  // scores). A member, not a per-call stack buffer.
  char txBuf[1536];
  JsonDocument doc;  // reused for every outgoing message (alloc-once-reuse)
};
