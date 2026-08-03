#pragma once
#include <string>
#include <vector>

#include "arcade/ArcadeSession.h"
#include "arcade/PackLoader.h"
#include "arcade/PartyGame.h"

// Word Scramble — first-correct wins. Server keeps the plaintext; phones send
// guess{text}. First correct = 200, then 120/80/40 by finishing order; rank >=4
// scores 0. Case-insensitive compare. Advances on deadline or all solved.
class ScrambleGame final : public PartyGame {
 public:
  static constexpr int WORD_CAP = 10;
  static constexpr int DUR_SECONDS = 30;
  static constexpr uint32_t RANK_POINTS[4] = {200, 120, 80, 40};

  bool load(const std::string& path);
  const char* packTitle() const { return packName; }

  const char* id() const override { return "scramble"; }
  int roundCount() const override { return static_cast<int>(words.size()); }
  void onRoundStart(int round) override;
  bool onClientIntent(uint8_t pid, const JsonDocument& msg) override;
  bool roundComplete() const override;
  void onReveal() override;
  void buildStateFor(uint8_t pid, JsonDocument& out) override;
  void buildFinal(JsonDocument& out) override;

  const char* title() const override { return "WORD SCRAMBLE"; }
  void roundLabel(char* buf, int len) const override;
  void tallyLine(char* buf, int len) const override;

 private:
  std::vector<PackLoader::WordItem> words;
  char packName[48] = {0};

  char scrambled[40] = {0};
  bool solved[ArcadeSession::MAX_CLIENTS];
  int rankNext = 0;
  bool anySolved = false;
  uint32_t deadline = 0;

  int solvedCount() const;
  static void scramble(const char* src, char* dst, size_t dstLen);
  static bool equalsIgnoreCase(const char* a, const char* b);
};
