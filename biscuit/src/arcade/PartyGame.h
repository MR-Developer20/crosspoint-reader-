#pragma once
#include <ArduinoJson.h>

#include <cstdint>

class ArcadeSession;

// Abstract base for every whole-group party game.
//
// ArcadeSession owns the roster, lobby, countdown, phase machine and all
// WebSocket broadcast. A PartyGame only fills the per-round bits: it never
// touches sockets, timers-of-record, or the roster directly (it reads the
// roster and awards points through the ArcadeSession pointer).
//
// The session drives games strictly event-driven from ArcadeHostActivity::loop():
//   onRoundStart(r)  -> set up round state, arm any deadline
//   onTick()         -> time-based internal transitions (react go-signal); returns
//                       true if clients must be re-broadcast
//   onClientIntent() -> a phone sent answer/guess/tap; returns true to rebroadcast
//   roundComplete()  -> deadline reached / all submitted / winner decided
//   onReveal()       -> score the round, flip to reveal payload
//   buildStateFor()  -> per-client state JSON (injects that player's own view)
//   buildFinal()     -> podium board
class PartyGame {
 public:
  virtual ~PartyGame() = default;

  void setSession(ArcadeSession* s) { session = s; }

  // Identity / structure
  virtual const char* id() const = 0;    // "trivia","wyr","scramble","react","gc"
  virtual int roundCount() const = 0;

  // Round lifecycle (all called on the single loop() thread)
  virtual void onRoundStart(int round) = 0;
  virtual bool onTick() { return false; }
  virtual bool onClientIntent(uint8_t pid, const JsonDocument& msg) = 0;
  virtual bool roundComplete() const = 0;
  virtual void onReveal() = 0;

  // Payload builders
  virtual void buildStateFor(uint8_t pid, JsonDocument& out) = 0;
  virtual void buildFinal(JsonDocument& out) = 0;

  // Host-screen hooks (near-static e-ink summary)
  virtual const char* title() const = 0;                 // "TRIVIA"
  virtual void roundLabel(char* buf, int len) const = 0; // "Q 3 / 10"
  virtual void tallyLine(char* buf, int len) const = 0;  // "Answered 4/6"

 protected:
  ArcadeSession* session = nullptr;
  bool revealing = false;  // set in onReveal(), cleared in onRoundStart()
  int roundIndex = 0;
};
