#pragma once
#include "arcade/ArcadeSession.h"
#include "arcade/PartyGame.h"

// Guess the Color — distance + speed. No SD content. Each round a random target
// color is shown as a swatch (numbers hidden); players dial an RGB guess.
// Scoring per §C: closeness = round(200*(1-dist/441.67)) clamped >=0,
// speed = round(100*(1-submit_ms/12000)) clamped >=0. 5 fixed rounds.
class GuessColorGame final : public PartyGame {
 public:
  static constexpr int ROUNDS = 5;
  static constexpr uint32_t ROUND_TIMEOUT_MS = 20000;
  static constexpr float MAX_DIST = 441.67f;    // sqrt(255^2 * 3)
  static constexpr float SPEED_WINDOW_MS = 12000.0f;

  const char* id() const override { return "gc"; }
  int roundCount() const override { return ROUNDS; }
  void onRoundStart(int round) override;
  bool onClientIntent(uint8_t pid, const JsonDocument& msg) override;
  bool roundComplete() const override;
  void onReveal() override;
  void buildStateFor(uint8_t pid, JsonDocument& out) override;
  void buildFinal(JsonDocument& out) override;

  const char* title() const override { return "GUESS THE COLOR"; }
  void roundLabel(char* buf, int len) const override;
  void tallyLine(char* buf, int len) const override;

 private:
  uint8_t tr = 0, tg = 0, tb = 0;  // target
  bool submitted[ArcadeSession::MAX_CLIENTS];
  uint8_t gr[ArcadeSession::MAX_CLIENTS], gg[ArcadeSession::MAX_CLIENTS], gb[ArcadeSession::MAX_CLIENTS];
  uint32_t submitMs[ArcadeSession::MAX_CLIENTS];
  int dist[ArcadeSession::MAX_CLIENTS];
  int points[ArcadeSession::MAX_CLIENTS];
  uint32_t startMs = 0;
  uint32_t deadline = 0;
  int winner = -1;

  int submittedCount() const;
  static void hex(uint8_t r, uint8_t g, uint8_t b, char* out);  // "#RRGGBB", out>=8
};
