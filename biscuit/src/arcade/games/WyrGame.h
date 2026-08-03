#pragma once
#include <string>
#include <vector>

#include "arcade/ArcadeSession.h"
#include "arcade/PackLoader.h"
#include "arcade/PartyGame.h"

// Would You Rather — an A/B poll. No scoring; just a live split. Rounds are
// prompts loaded from an SD pack. Advances when all voted or a soft deadline.
class WyrGame final : public PartyGame {
 public:
  static constexpr int PROMPT_CAP = 8;
  static constexpr uint32_t SOFT_DEADLINE_MS = 25000;

  bool load(const std::string& path);
  const char* packTitle() const { return packName; }

  const char* id() const override { return "wyr"; }
  int roundCount() const override { return static_cast<int>(prompts.size()); }
  void onRoundStart(int round) override;
  bool onClientIntent(uint8_t pid, const JsonDocument& msg) override;
  bool roundComplete() const override;
  void onReveal() override;
  void buildStateFor(uint8_t pid, JsonDocument& out) override;
  void buildFinal(JsonDocument& out) override;

  const char* title() const override { return "WOULD YOU RATHER"; }
  void roundLabel(char* buf, int len) const override;
  void tallyLine(char* buf, int len) const override;

 private:
  std::vector<PackLoader::AbItem> prompts;
  char packName[48] = {0};

  int8_t vote[ArcadeSession::MAX_CLIENTS];  // -1 / 0 / 1
  uint16_t counts[2];
  uint32_t deadline = 0;

  int votedCount() const;
};
