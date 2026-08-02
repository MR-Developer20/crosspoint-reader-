#pragma once
#include <vector>

#include "ArcadeSession.h"
#include "PackLoader.h"
#include "PartyGame.h"

// Trivia: N multiple-choice questions from an SD pack. Server-authoritative,
// integer-only scoring (correct base + speed bonus). Advancement is a deadline
// check driven by ArcadeSession::tick() — no per-frame tick, no thread.
class TriviaGame final : public PartyGame {
 public:
  static constexpr int QUESTION_CAP = 40;
  static constexpr int DUR_SECONDS = 20;
  static constexpr uint32_t BASE_POINTS = 500;
  static constexpr uint32_t MAX_SPEED_BONUS = 500;

  // Load the pack up-front; host checks the return before starting the game.
  bool load(const std::string& path);
  const char* packTitle() const { return title; }

  const char* id() const override { return "trivia"; }
  int roundCount() const override { return static_cast<int>(questions.size()); }
  void onRoundStart(int round) override;
  bool onClientIntent(uint8_t pid, const JsonDocument& msg) override;
  bool roundComplete() const override;
  void onReveal() override;
  void buildStateFor(uint8_t pid, JsonDocument& out) override;
  void buildFinal(JsonDocument& out) override;

  const char* title() const override { return "TRIVIA"; }
  void roundLabel(char* buf, int len) const override;
  void tallyLine(char* buf, int len) const override;

 private:
  std::vector<PackLoader::TriviaQ> questions;
  char title[48] = {0};

  int8_t choice[ArcadeSession::MAX_CLIENTS];
  uint32_t answerMs[ArcadeSession::MAX_CLIENTS];
  bool answered[ArcadeSession::MAX_CLIENTS];
  uint16_t counts[4];
  uint32_t deadline = 0;

  int answeredCount() const;
};
