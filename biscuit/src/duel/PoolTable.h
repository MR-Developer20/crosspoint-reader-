#pragma once
#include <cstdint>

// Authoritative 8-ball state + rules referee.
//
// The ESP32-C3 runs NO physics. Pool is turn-based, so each shot is a single
// event: the shooting phone simulates the shot locally and reports the outcome
// (final ball positions, first ball contacted, whether a rail was hit after
// contact). PoolTable adopts those positions and applies the 8-ball RULES —
// group assignment, fouls, ball-in-hand, win/loss — which is the part that must
// stay server-authoritative so the two phones can't disagree.
//
// Ball ids: 0 = cue, 1..7 = solids, 8 = eight, 9..15 = stripes.
class PoolTable {
 public:
  static constexpr int NUM_BALLS = 16;

  enum class Phase : uint8_t { WAITING, PLAYING, GAMEOVER };
  enum class Group : uint8_t { OPEN, SOLIDS, STRIPES };

  struct Ball {
    float x = 0;
    float y = 0;
    bool pocketed = false;
  };

  // Table is normalised: playfield 1.0 wide x 0.5 tall (2:1). The client scales.
  static constexpr float W = 1.0f;
  static constexpr float H = 0.5f;

  Ball balls[NUM_BALLS];
  Phase phase = Phase::WAITING;
  Group groups[2] = {Group::OPEN, Group::OPEN};
  int turn = 0;            // seat 0/1 to shoot
  bool ballInHand = false;  // current shooter may reposition the cue
  bool onBreak = false;
  int winner = -1;          // seat, when GAMEOVER
  char message[40] = {0};

  // Replay hint for opponent/spectator animation (they re-simulate from their
  // synced pre-shot positions, then snap to the authoritative final).
  uint32_t shotSeq = 0;
  int lastBy = -1;
  float lastCueX = 0, lastCueY = 0, lastVX = 0, lastVY = 0;

  // Rack the balls and start a game; `breaker` seat shoots first.
  void rack(int breaker);

  // Apply a reported shot from `seat`. `nb` are the shooter's final positions
  // (index = ball id), `firstHit` the first object ball the cue struck (-1 = no
  // contact), `rail` whether any ball hit a cushion after that contact. `vx/vy`
  // are the cue's launch velocity (for spectator replay).
  void applyShot(int seat, const Ball nb[NUM_BALLS], int firstHit, bool rail, float vx, float vy);

  Group groupOfBall(int id) const;   // OPEN meaning "neither" for cue/8
  bool isGroupBall(int id) const { return (id >= 1 && id <= 7) || (id >= 9 && id <= 15); }
  bool clearedGroup(int seat) const;  // all of the seat's group pocketed

 private:
  void setMessage(const char* m);
  void assignGroups(int seat, const bool newlyPk[NUM_BALLS]);
  void placeCueAtHeadSpot();
};
