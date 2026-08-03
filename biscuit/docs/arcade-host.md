# Arcade Host — implementation notes

Native Biscuit recreation of the Flipper *Hotspot Arcade* whole-group party games.
The X4 (ESP32-C3) hosts an **open WiFi AP + captive redirect + HTTP page + a
WebSocket server**; phones join and play in their browsers. One `Activity` is the
entire referee — AP, web server, WebSocket termination and the game engine — all
single-threaded in `loop()`. No BLE, no FreeRTOS tasks, no mutexes.

## Files

```
src/activities/apps/ArcadeHostActivity.{h,cpp}   AP/DNS/HTTP/WS bring-up, host UI, WS trampoline
src/arcade/ArcadeSession.{h,cpp}                 roster + lobby + countdown + phase machine + broadcast
src/arcade/PartyGame.h                            abstract per-game interface
src/arcade/PackLoader.{h,cpp}                     SD pack parser (trivia / A-B / word), Flashcard-style
src/arcade/TriviaGame.{h,cpp}                     Trivia
src/arcade/games/WyrGame.{h,cpp}                  Would You Rather (poll)
src/arcade/games/ScrambleGame.{h,cpp}             Word Scramble (first-correct)
src/arcade/games/ReactGame.{h,cpp}                Reaction Duel (generated)
src/arcade/games/GuessColorGame.{h,cpp}           Guess the Color (generated)
src/network/html/arcade.html                      phone client for all five games (gzipped into flash by build_html.py)
docs/arcade-packs/                                sample SD packs + install guide
```

Registered under **Games → Arcade Host** (`AppsMenuActivity.cpp`, category 5).

## Architecture

`ArcadeHostActivity` mirrors `CaptivePortalActivity` for the radio/AP/DNS/HTTP
bring-up, adds a `WebSocketsServer` on port 81 (links2004, C-style static
trampoline → instance, exactly like `CrossPointWebServer`), and serves the phone
client straight from flash (`send_P` + `Content-Encoding: gzip`, zero heap).

`ArcadeSession` is **game-agnostic**: it owns a fixed 6-slot roster, the lobby,
the 5-second countdown, the round/reveal/final phase machine and all per-client
JSON broadcast. Games implement `PartyGame` and only fill the per-round bits.
All advancement is a `millis()` deadline / all-submitted check in `tick()` (plus
React's random go-signal via `onTick()`) — never a per-frame tick, thread or mutex.

The e-ink host screen is **near-static**: `ArcadeSession` raises a `dirty` flag on
roster/phase changes only (never on countdown ticks), and the activity redraws
only when it is set.

Client cap: `ArcadeSession::MAX_CLIENTS = 6`. The links2004 compile-time cap is set
one higher (`-DWEBSOCKETS_SERVER_CLIENT_MAX=8` in `platformio.ini`) so the overflow
socket can be accepted and then cleanly rejected with a `toast{"Lobby full"}`.

## Protocol (WebSocket JSON, port 81)

Every message is `{"t":<type>, …}`. See §A of the project spec; the client
(`arcade.html`) and server implement the same contract:

- client→server: `hello{nick}`, `ready{ready}`, `answer{c}`, `guess{text}`,
  `guess{r,g,b}`, `tap`, `ping{n}` / `pong`.
- server→client: `welcome`, `lobby`, `countdown{n}`, `toast{msg}`, `pong{n,heap}`,
  and per-game `trivia` / `wyr` / `scramble` / `react` / `gc`, each with a
  `phase:"final"` + `board` variant for the podium.

`guess` is routed by fields present (`text` → Scramble, `r,g,b` → Guess-the-Color).

## Scoring (server-authoritative, integer-only — §C)

- **Trivia**: correct → `500` base + speed bonus up to `500` (linear in time left).
- **Word Scramble**: first correct `200`, then `120 / 80 / 40`; rank ≥4 → `0`.
- **Reaction Duel**: first valid tap after "go" → `200`; tap during "wait" → DQ (0).
- **Guess the Color**: `closeness = round(200·(1−dist/441.67))` + `speed =
  round(100·(1−submit_ms/12000))`, both clamped ≥ 0.
- **Would You Rather**: no scoring (poll).

## Memory notes

- Fixed-size roster (`Player[6]`), no growing vectors for players.
- One reused `JsonDocument` + a `char txBuf[1536]` per session for all outgoing
  messages (alloc-once-reuse).
- Pack loaders `reserve()` and **cap**: Trivia 40 Q, WYR 8, Scramble 10; extra
  blocks are skipped and logged.
- The web bundle is served from flash (`send_P`), never buffered in heap.

## On-hardware verification (requires flashing — not done in this change)

This project is **RAM-gated**. The caps above are the spec's hypotheses; the
real ceiling is whatever the device measures. These steps must be run on an X4:

1. Build/flash `env:default`, watch `python3 scripts/debugging_monitor.py`.
2. Copy `docs/arcade-packs/*` to the SD card (`/biscuit/arcade/{trivia,wyr,scramble}`).
3. Ramp to **6 phones**, record `Min heap` (shown on the host lobby screen) at
   1→6 clients. Provisional go: ≥ 40 KB free; below ~20 KB is a hard no-go.
4. Confirm the **7th** socket is rejected ("Lobby full") and the other six are
   unaffected; hold 6 phones ≥ 5 min for watchdog/heap-decay.
5. Play a full pack of each game with 6 phones; confirm phones ↔ host podium agree.
6. **Back** tears down the AP and releases the radio — re-enter to check for leaks.
