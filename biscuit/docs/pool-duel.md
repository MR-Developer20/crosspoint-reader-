# 8-Ball Pool — 1v1 duel

A phone-hosted 8-ball pool duel. The X4 (ESP32-C3) runs an open WiFi AP + captive
redirect + the phone client (served gzipped from flash) + a WebSocket table on
:81. Two players are seated; everyone else spectates. Single-threaded, no BLE,
no FreeRTOS tasks. Registered under **Games → 8-Ball Pool**.

## Why the ESP runs no physics

Pool is real-time/continuous — the exact class of game the whole-group arcade
plan defers, because simulating balls and streaming positions many times per
second does not fit the single-core, event-driven, near-static-e-ink model.

The fix that makes it viable: **pool is turn-based, so each shot is one event.**
The physics runs on the *shooting player's phone* (which has ample CPU); the ESP
is only the **rules referee + relay**:

- The shooter aims and shoots; their phone simulates the shot locally and reports
  the outcome — final ball positions, the first object ball contacted, and
  whether a rail was hit after contact.
- `PoolTable` (server) adopts those positions and applies 8-ball **rules**:
  group assignment (open table → solids/stripes), fouls → ball-in-hand,
  must-hit-your-group-first, no-rail fouls, and 8-ball win/loss.
- The new state is broadcast to everyone. The opponent and spectators replay the
  shot locally from their synced pre-shot positions (using the reported cue
  velocity), then snap to the authoritative final positions.

This keeps the ESP event-driven and low-bandwidth. The tradeoff, by design:
ball geometry is **client-computed**, so the ESP trusts the shooter's reported
positions — fine for friends around a table. Rules (turn, group, foul, win) stay
server-authoritative.

## Files

```
src/activities/apps/PoolDuelActivity.{h,cpp}   AP/DNS/HTTP/WS bring-up, host screen, WS trampoline
src/duel/PoolTable.{h,cpp}                     authoritative 8-ball state + rules (no physics)
src/duel/DuelSession.{h,cpp}                   2 seats + spectators, turn relay, per-client broadcast
src/network/html/pool.html                     phone client: canvas physics + aim UI + spectator replay
```

## Protocol (WebSocket JSON, port 81)

- client→server: `hello{nick}`, `sit`, `stand`, `start`, `shot{balls[],firstHit,rail,vx,vy}`,
  `rematch`, `ping{n}` / `pong`.
- server→client: `state{...}` (per client, includes `you`/`yourTurn`), `toast{msg}`,
  `pong{n,heap}`. `state` carries the full table (balls, groups, turn, phase,
  ballInHand, winner, message) plus a replay hint (`shotSeq`, `lastBy`,
  `lcx/lcy/lvx/lvy`).

Ball ids: 0 = cue, 1–7 solids, 8 = eight, 9–15 stripes. Table is normalised
1.0 × 0.5; the phone scales it to the canvas.

## Playing

1. Games → **8-Ball Pool** → enter an AP name (default `POOL`) → the AP starts.
2. Players join the open WiFi (scan the QR), enter a name; the first two are
   seated, the rest spectate. A spectator can **Take Seat** if one is open.
3. Host presses **Start** (or a seated player taps Start) → break.
4. Play alternates by turn; fouls give ball-in-hand. Pot the 8 legally after
   clearing your group to win. **Rematch** (loser breaks) or **Back** to stop.

## Rules implemented

Open-table group assignment on the first legally pocketed object ball;
must-hit-your-own-group-first (or the 8 once your group is cleared); cue scratch,
wrong-ball-first, and no-rail-after-contact fouls → opponent ball-in-hand; 8 on
the break re-racks; potting the 8 early or on a foul loses. Called-pocket and
some tournament edge cases are intentionally omitted (party ruleset).

## On-hardware verification (requires flashing)

Same shared transport as the arcade. On an X4: launch, join with 2 phones + a
spectator, play a full game (break → groups → fouls/ball-in-hand → 8-ball win),
watch `MinHeap` on the host screen, then **Back** to confirm the radio/heap are
released cleanly on exit.
