#include "PoolTable.h"

#include <cstring>

void PoolTable::setMessage(const char* m) {
  strncpy(message, m, sizeof(message) - 1);
  message[sizeof(message) - 1] = '\0';
}

PoolTable::Group PoolTable::groupOfBall(int id) const {
  if (id >= 1 && id <= 7) return Group::SOLIDS;
  if (id >= 9 && id <= 15) return Group::STRIPES;
  return Group::OPEN;  // cue (0) or eight (8)
}

bool PoolTable::clearedGroup(int seat) const {
  Group g = groups[seat];
  if (g == Group::OPEN) return false;
  int lo = (g == Group::SOLIDS) ? 1 : 9;
  for (int id = lo; id <= lo + 6; ++id)
    if (!balls[id].pocketed) return false;
  return true;
}

void PoolTable::placeCueAtHeadSpot() {
  balls[0].pocketed = false;
  balls[0].x = 0.25f * W;
  balls[0].y = 0.5f * H;
}

void PoolTable::rack(int breaker) {
  for (auto& b : balls) b = Ball{};

  // Cue on the head spot.
  balls[0] = {0.25f * W, 0.5f * H, false};

  // Standard triangle at the foot spot: apex toward the cue, 8-ball in the
  // centre of the third row, corners a solid and a stripe. Exact arrangement is
  // cosmetic — the break scatters everything (physics runs on the phone).
  const float apexX = 0.72f * W;
  const float cy = 0.5f * H;
  const float dx = 0.038f;  // row spacing
  const float dy = 0.021f;  // vertical spacing between neighbours
  // Rows of 1,2,3,4,5. Ball ids laid out with 8 in the middle of row 3.
  static const int layout[5][5] = {
      {1, -1, -1, -1, -1},
      {9, 2, -1, -1, -1},
      {10, 8, 3, -1, -1},
      {11, 4, 12, 5, -1},
      {13, 6, 14, 7, 15},
  };
  for (int row = 0; row < 5; ++row) {
    int count = row + 1;
    float x = apexX + row * dx;
    float y0 = cy - (count - 1) * dy * 0.5f;
    for (int i = 0; i < count; ++i) {
      int id = layout[row][i];
      if (id < 0) continue;
      balls[id] = {x, y0 + i * dy, false};
    }
  }

  phase = Phase::PLAYING;
  groups[0] = groups[1] = Group::OPEN;
  turn = breaker;
  ballInHand = false;
  onBreak = true;
  winner = -1;
  shotSeq++;
  lastBy = -1;
  setMessage("Break!");
}

void PoolTable::assignGroups(int seat, const bool newlyPk[NUM_BALLS]) {
  for (int id = 1; id <= 15; ++id) {
    if (id == 8 || !newlyPk[id]) continue;
    Group g = groupOfBall(id);
    if (g == Group::OPEN) continue;
    groups[seat] = g;
    groups[1 - seat] = (g == Group::SOLIDS) ? Group::STRIPES : Group::SOLIDS;
    return;
  }
}

void PoolTable::applyShot(int seat, const Ball nb[NUM_BALLS], int firstHit, bool rail, float vx, float vy) {
  if (phase != Phase::PLAYING || seat != turn) return;

  bool wasPk[NUM_BALLS];
  for (int i = 0; i < NUM_BALLS; ++i) wasPk[i] = balls[i].pocketed;

  // Adopt the shooter's reported positions (authoritative for geometry).
  for (int i = 0; i < NUM_BALLS; ++i) balls[i] = nb[i];

  bool newlyPk[NUM_BALLS];
  for (int i = 0; i < NUM_BALLS; ++i) newlyPk[i] = balls[i].pocketed && !wasPk[i];

  const bool cueScratch = balls[0].pocketed;
  const bool eightPk = newlyPk[8];
  const int other = 1 - seat;

  // Record replay hint before we mutate turn/state.
  shotSeq++;
  lastBy = seat;
  lastCueX = nb[0].x;
  lastCueY = nb[0].y;
  lastVX = vx;
  lastVY = vy;

  bool foul = false;
  const char* msg = "";

  // ---- Break shot ----
  if (onBreak) {
    onBreak = false;
    if (eightPk) {
      // 8 on the break: simplest fair ruleset — re-rack, same breaker.
      rack(seat);
      setMessage("8 on break - re-rack");
      return;
    }
    if (firstHit < 0) foul = true;          // whiff
    if (cueScratch) foul = true;            // scratch on break
    bool pottedAny = false;
    for (int id = 1; id <= 15; ++id)
      if (id != 8 && newlyPk[id]) pottedAny = true;

    if (cueScratch) placeCueAtHeadSpot();
    if (foul) {
      ballInHand = true;
      turn = other;
      setMessage(cueScratch ? "Scratch - ball in hand" : "Foul - ball in hand");
    } else if (pottedAny) {
      ballInHand = false;
      turn = seat;  // table still open; breaker continues
      setMessage("Table open");
    } else {
      ballInHand = false;
      turn = other;
      setMessage("Table open");
    }
    return;
  }

  // ---- Normal shot: legality ----
  if (firstHit < 0) {
    foul = true;
    msg = "No ball hit - foul";
  } else if (groups[seat] == Group::OPEN) {
    if (firstHit == 8) {
      foul = true;
      msg = "Hit the 8 first - foul";
    }
  } else {
    bool onEight = clearedGroup(seat);
    if (onEight) {
      if (firstHit != 8) {
        foul = true;
        msg = "Must hit the 8 - foul";
      }
    } else if (groupOfBall(firstHit) != groups[seat]) {
      foul = true;
      msg = "Wrong ball first - foul";
    }
  }
  if (cueScratch) {
    foul = true;
    msg = "Scratch - ball in hand";
  }
  // No rail after a legal contact and nothing pocketed → table foul.
  if (!foul && firstHit >= 0 && !rail) {
    bool pottedAny = false;
    for (int id = 1; id <= 15; ++id)
      if (newlyPk[id]) pottedAny = true;
    if (!pottedAny) {
      foul = true;
      msg = "No rail - foul";
    }
  }

  // ---- 8-ball pocketed: decides the game ----
  if (eightPk) {
    bool legalWin = clearedGroup(seat) && !foul && !cueScratch;
    winner = legalWin ? seat : other;
    phase = Phase::GAMEOVER;
    setMessage(legalWin ? "8-ball - winner!" : "8-ball early - loss");
    return;
  }

  // ---- Assign groups on an open table (first legally pocketed object ball) ----
  if (groups[seat] == Group::OPEN && !foul) assignGroups(seat, newlyPk);

  // ---- Did the shooter pocket one of their own group? ----
  bool pottedOwn = false;
  if (groups[seat] != Group::OPEN) {
    for (int id = 1; id <= 15; ++id)
      if (id != 8 && newlyPk[id] && groupOfBall(id) == groups[seat]) pottedOwn = true;
  } else {
    for (int id = 1; id <= 15; ++id)
      if (id != 8 && newlyPk[id]) pottedOwn = true;  // open table: any pot keeps the turn
  }

  if (cueScratch) placeCueAtHeadSpot();

  if (foul) {
    ballInHand = true;
    turn = other;
    setMessage(msg[0] ? msg : "Foul - ball in hand");
  } else if (pottedOwn) {
    ballInHand = false;
    turn = seat;
    setMessage("Potted - shoot again");
  } else {
    ballInHand = false;
    turn = other;
    setMessage("Your shot");
  }
}
