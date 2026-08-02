# Arcade Host — sample content packs

The **Arcade Host** app (Games → Arcade Host) hosts an offline WiFi party arcade:
the X4 runs an open WiFi access point, phones join and play together in their
browsers (no internet, no install). Five whole-group games are included:

| Game | Content | Scored |
|---|---|---|
| Trivia | SD pack (`Q/A–D/Answer`) | Yes (correct + speed) |
| Would You Rather | SD pack (`A/B`) | No — live poll |
| Word Scramble | SD pack (`Word`) | Yes (`200/120/80/40` by finish order) |
| Reaction Duel | generated | Yes (`200` to the fastest tap) |
| Guess the Color | generated | Yes (color distance + speed) |

Reaction Duel and Guess the Color need no content. The other three read `*.txt`
packs from the SD card.

## Installing packs

Copy the folders in this directory to the **SD card** so the layout becomes:

```
/biscuit/arcade/trivia/general.txt
/biscuit/arcade/wyr/everyday.txt
/biscuit/arcade/scramble/classic.txt
```

The app creates the `/biscuit/arcade/{trivia,wyr,scramble}` folders on first run
if they do not exist — you can also just drop your own `.txt` files into them.

## Pack format (byte-compatible with Flipper Hotspot Arcade)

Blocks are separated by a line containing only `---`. The first `Pack: <name>`
line is the display title. Fields are fixed (`Q:`, `A:`–`D:`, `Answer:`, `Word:`).

**Trivia** — `/biscuit/arcade/trivia/*.txt`
```
Pack: General Knowledge
Q: What is the capital of France?
A: Paris
B: London
C: Berlin
D: Madrid
Answer: A
---
```

**Would You Rather** — `/biscuit/arcade/wyr/*.txt`
```
Pack: Everyday
A: Be able to fly
B: Be invisible
---
```

**Word Scramble** — `/biscuit/arcade/scramble/*.txt`
```
Pack: Classic
Word: planet
---
```

## Limits

To respect the ESP32-C3's ~380 KB RAM, packs are capped when loaded: Trivia 40
questions, Would You Rather 8 prompts, Word Scramble 10 words. Extra blocks are
skipped (logged over serial). Concurrent players are capped at 6.

## How to play

1. Games → **Arcade Host** → enter an AP name (default `ARCADE`) → the AP starts.
2. Pick a game on the device (Trivia/WYR/Scramble also pick a pack).
3. Players join the open `ARCADE` WiFi (scan the QR on screen) and are captive-
   redirected to the game page, enter a nickname, and tap **Ready**.
4. When everyone is ready (≥2 players), a 5-second countdown starts the game.
5. **Back** on the device stops the arcade and releases the radio.
