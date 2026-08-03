#pragma once
#include "arcade/ArcadeSession.h"
#include "arcade/PartyGame.h"

// Reaction Duel — fastest finger. No SD content. Each round arms after a random
// 1.5–5 s delay ("wait" → "go"); the first valid tap after "go" wins 200 pts.
// Tapping during "wait" DQs the player for the round. All timing keys off
// millis() in the single loop() thread (onTick flips the light).
class ReactGame final : public PartyGame {
 public:
  static constexpr int ROUNDS = 5;
  static constexpr uint32_t WINNER_POINTS = 200;
  static constexpr long GO_MIN_MS = 1500;
  static constexpr long GO_MAX_MS = 5000;
  static constexpr uint32_t GO_WINDOW_MS = 8000;  // max wait for a tap after "go"

  const char* id() const override { return "react"; }
  int roundCount() const override { return ROUNDS; }
  void onRoundStart(int round) override;
  bool onTick() override;
  bool onClientIntent(uint8_t pid, const JsonDocument& msg) override;
  bool roundComplete() const override;
  void onReveal() override;
  void buildStateFor(uint8_t pid, JsonDocument& out) override;
  void buildFinal(JsonDocument& out) override;

  const char* title() const override { return "REACTION DUEL"; }
  void roundLabel(char* buf, int len) const override;
  void tallyLine(char* buf, int len) const override;

 private:
  bool go = false;  // false = "wait", true = "go"
  uint32_t goAt = 0;
  uint32_t goMs = 0;
  bool dq[ArcadeSession::MAX_CLIENTS];
  bool tapped[ArcadeSession::MAX_CLIENTS];
  int winner = -1;
  uint32_t winnerMs = 0;

  bool allResolved() const;  // every active player is DQ'd or has tapped
};
