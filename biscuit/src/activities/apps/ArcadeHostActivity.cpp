#include "ArcadeHostActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "arcade/PackLoader.h"
#include "arcade/TriviaGame.h"
#include "arcade/games/GuessColorGame.h"
#include "arcade/games/ReactGame.h"
#include "arcade/games/ScrambleGame.h"
#include "arcade/games/WyrGame.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/QrUtils.h"
#include "util/RadioManager.h"

// The gzipped phone client, baked into flash by scripts/build_html.py from
// src/network/html/arcade.html at build time (served with Content-Encoding: gzip
// straight from flash → zero heap cost).
#include "network/html/arcadeHtml.generated.h"

namespace {
// Game registry: id → { display name, pack directory (nullptr = generated) }.
struct GameDef {
  const char* name;
  const char* packDir;  // nullptr for React / Guess-the-Color (no SD content)
};
constexpr GameDef GAMES[] = {
    {"Trivia", "/biscuit/arcade/trivia"},
    {"Would You Rather", "/biscuit/arcade/wyr"},
    {"Word Scramble", "/biscuit/arcade/scramble"},
    {"Reaction Duel", nullptr},
    {"Guess the Color", nullptr},
};
constexpr int GAME_COUNT = sizeof(GAMES) / sizeof(GAMES[0]);

ArcadeHostActivity* arcadeInstance = nullptr;
}  // namespace

// ---------------------------------------------------------------- lifecycle

void ArcadeHostActivity::onEnter() {
  Activity::onEnter();
  arcadeInstance = this;
  RADIO.ensureWifi();
  randomSeed(esp_random());

  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/arcade");
  Storage.mkdir("/biscuit/arcade/trivia");
  Storage.mkdir("/biscuit/arcade/wyr");
  Storage.mkdir("/biscuit/arcade/scramble");

  state = ENTER_SSID;
  hostView = HV_PICK_GAME;
  gameSel = 0;
  statusMsg[0] = '\0';
  promptSsid();
}

void ArcadeHostActivity::onExit() {
  Activity::onExit();
  stopHost();
  arcadeInstance = nullptr;
  RADIO.shutdown();
}

void ArcadeHostActivity::promptSsid() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "AP Name", apSsid, 24),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          finish();
          return;
        }
        std::string entered = std::get<KeyboardResult>(result.data).text;
        if (!entered.empty()) apSsid = entered;
        startHost();
        requestUpdate();
      });
}

// ---------------------------------------------------------------- bring-up

void ArcadeHostActivity::startHost() {
  LOG_DBG("ARC", "[MEM] Free heap before start: %d", ESP.getFreeHeap());

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str());  // open network, no password
  delay(100);

  dnsServer = std::make_unique<DNSServer>();
  dnsServer->start(53, "*", WiFi.softAPIP());

  httpServer = std::make_unique<WebServer>(80);
  httpServer->on("/", HTTP_GET, [this] { handleRoot(); });
  httpServer->onNotFound([this] { handleNotFound(); });
  httpServer->begin();

  wsServer = std::make_unique<WebSocketsServer>(81);
  wsServer->onEvent(ArcadeHostActivity::wsTrampoline);
  wsServer->begin();

  session.begin(wsServer.get());

  state = RUNNING;
  hostView = HV_PICK_GAME;
  LOG_DBG("ARC", "AP '%s' up; http:80 ws:81; free heap: %d", apSsid.c_str(), ESP.getFreeHeap());
}

void ArcadeHostActivity::stopHost() {
  if (wsServer) {
    wsServer->close();
    wsServer.reset();
  }
  if (httpServer) {
    httpServer->stop();
    httpServer.reset();
  }
  if (dnsServer) {
    dnsServer->stop();
    dnsServer.reset();
  }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  LOG_DBG("ARC", "[MEM] Free heap after stop: %d", ESP.getFreeHeap());
}

void ArcadeHostActivity::handleRoot() {
  httpServer->sendHeader("Content-Encoding", "gzip");
  httpServer->send_P(200, "text/html", arcadeHtml, arcadeHtmlCompressedSize);
}

void ArcadeHostActivity::handleNotFound() {
  // Captive redirect: bounce every unknown host to the game page.
  httpServer->sendHeader("Location", "http://192.168.4.1/", true);
  httpServer->send(302, "text/plain", "");
}

// ---------------------------------------------------------------- websocket

void ArcadeHostActivity::wsTrampoline(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (arcadeInstance) arcadeInstance->onWsEvent(num, type, payload, length);
}

void ArcadeHostActivity::onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      if (!session.onConnect(num) && wsServer) {
        wsServer->disconnect(num);  // over MAX_CLIENTS — cleanly reject the overflow socket
      }
      break;
    case WStype_DISCONNECTED:
      session.onDisconnect(num);
      break;
    case WStype_TEXT:
      session.onText(num, reinterpret_cast<const char*>(payload), length);
      break;
    case WStype_BIN:
    default:
      break;  // no binary path in the arcade protocol
  }
}

// ---------------------------------------------------------------- game select

void ArcadeHostActivity::onGameChosen(int idx) {
  selectedGameIdx = idx;
  statusMsg[0] = '\0';
  const GameDef& g = GAMES[idx];
  if (g.packDir) {
    packs = PackLoader::scanPacks(g.packDir);
    if (packs.empty()) {
      snprintf(statusMsg, sizeof(statusMsg), "No packs in %s", g.packDir);
      requestUpdate();
      return;
    }
    packSel = 0;
    hostView = HV_PICK_PACK;
    requestUpdate();
    return;
  }
  // Generated games — no pack.
  std::unique_ptr<PartyGame> game;
  if (idx == 3)
    game = std::make_unique<ReactGame>();
  else
    game = std::make_unique<GuessColorGame>();
  session.setGame(std::move(game));
  hostView = HV_LOBBY;
  requestUpdate();
}

void ArcadeHostActivity::onPackChosen() {
  const std::string& path = packs[packSel];
  bool ok = false;
  std::unique_ptr<PartyGame> game;
  switch (selectedGameIdx) {
    case 0: {
      auto t = std::make_unique<TriviaGame>();
      ok = t->load(path);
      game = std::move(t);
      break;
    }
    case 1: {
      auto w = std::make_unique<WyrGame>();
      ok = w->load(path);
      game = std::move(w);
      break;
    }
    case 2: {
      auto s = std::make_unique<ScrambleGame>();
      ok = s->load(path);
      game = std::move(s);
      break;
    }
    default:
      return;
  }
  if (!ok) {
    snprintf(statusMsg, sizeof(statusMsg), "Pack empty or unreadable");
    requestUpdate();
    return;
  }
  session.setGame(std::move(game));
  hostView = HV_LOBBY;
  requestUpdate();
}

// ---------------------------------------------------------------- loop

void ArcadeHostActivity::loop() {
  if (state != RUNNING) {
    if (state == STOPPED && mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  // Service the servers and advance the game — all on this one thread.
  dnsServer->processNextRequest();
  httpServer->handleClient();
  wsServer->loop();
  session.tick();

  using B = MappedInputManager::Button;
  const ArcadeSession::Phase phase = session.getPhase();

  if (phase == ArcadeSession::Phase::LOBBY) {
    switch (hostView) {
      case HV_LOBBY:
        if (mappedInput.wasReleased(B::Back)) {
          stopHost();
          state = STOPPED;
          requestUpdate();
          return;
        }
        if (mappedInput.wasReleased(B::Confirm)) {
          hostView = HV_PICK_GAME;
          gameSel = 0;
          requestUpdate();
        }
        break;
      case HV_PICK_GAME:
        if (mappedInput.wasReleased(B::Back)) {
          hostView = HV_LOBBY;
          requestUpdate();
        } else if (mappedInput.wasReleased(B::Up)) {
          gameSel = ButtonNavigator::previousIndex(gameSel, GAME_COUNT);
          requestUpdate();
        } else if (mappedInput.wasReleased(B::Down)) {
          gameSel = ButtonNavigator::nextIndex(gameSel, GAME_COUNT);
          requestUpdate();
        } else if (mappedInput.wasReleased(B::Confirm)) {
          onGameChosen(gameSel);
        }
        break;
      case HV_PICK_PACK:
        if (mappedInput.wasReleased(B::Back)) {
          hostView = HV_PICK_GAME;
          requestUpdate();
        } else if (mappedInput.wasReleased(B::Up)) {
          packSel = ButtonNavigator::previousIndex(packSel, (int)packs.size());
          requestUpdate();
        } else if (mappedInput.wasReleased(B::Down)) {
          packSel = ButtonNavigator::nextIndex(packSel, (int)packs.size());
          requestUpdate();
        } else if (mappedInput.wasReleased(B::Confirm)) {
          onPackChosen();
        }
        break;
    }
    if (session.consumeDirty()) requestUpdate();
    return;
  }

  // In-game phases: Back always stops; OK is context-sensitive.
  if (mappedInput.wasReleased(B::Back)) {
    stopHost();
    state = STOPPED;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(B::Confirm)) {
    if (phase == ArcadeSession::Phase::ROUND)
      session.hostReveal();
    else if (phase == ArcadeSession::Phase::REVEAL || phase == ArcadeSession::Phase::FINAL) {
      session.hostAdvance();
      if (session.getPhase() == ArcadeSession::Phase::LOBBY) hostView = HV_LOBBY;
    }
  }

  if (session.consumeDirty()) requestUpdate();
}

// ---------------------------------------------------------------- rendering

void ArcadeHostActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  char header[40];
  snprintf(header, sizeof(header), "ARCADE - %s", apSsid.c_str());
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header);

  if (state == ENTER_SSID) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, "Configuring...");
    renderer.displayBuffer();
    return;
  }
  if (state == STOPPED) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, "Stopped. Radio released.");
    const auto labels = mappedInput.mapLabels("Exit", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  switch (session.getPhase()) {
    case ArcadeSession::Phase::LOBBY:
      if (hostView == HV_PICK_GAME)
        renderPickGame();
      else if (hostView == HV_PICK_PACK)
        renderPickPack();
      else
        renderLobby();
      break;
    case ArcadeSession::Phase::COUNTDOWN:
      renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, "GET READY!", true,
                                EpdFontFamily::BOLD);
      break;
    case ArcadeSession::Phase::ROUND:
      renderRound();
      break;
    case ArcadeSession::Phase::REVEAL:
      renderReveal();
      break;
    case ArcadeSession::Phase::FINAL:
      renderPodium();
      break;
  }
  renderer.displayBuffer();
}

void ArcadeHostActivity::drawJoinQr() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + 10;
  const int size = 180;
  const int x = renderer.getScreenWidth() - size - metrics.contentSidePadding;
  std::string payload = "WIFI:T:nopass;S:" + apSsid + ";;";
  QrUtils::drawQrCode(renderer, Rect{x, top, size, size}, payload);
  renderer.drawText(SMALL_FONT_ID, x, top + size + 6, "Scan to join");
}

void ArcadeHostActivity::renderLobby() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int leftPad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + 20;
  const int lineH = 34;

  drawJoinQr();

  char buf[64];
  const char* gname = session.hasGame() ? GAMES[selectedGameIdx >= 0 ? selectedGameIdx : 0].name : "(pick a game)";
  snprintf(buf, sizeof(buf), "Game: %s", gname);
  renderer.drawText(UI_12_FONT_ID, leftPad, y, buf, true, EpdFontFamily::BOLD);
  y += lineH;

  snprintf(buf, sizeof(buf), "Players: %d/%d   Ready: %d", session.playerCount(), ArcadeSession::MAX_CLIENTS,
           session.readyCount());
  renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
  y += lineH;

  for (int i = 0; i < ArcadeSession::MAX_CLIENTS; ++i) {
    if (!session.slotActive(i)) continue;
    const ArcadeSession::Player& p = session.playerAt(i);
    snprintf(buf, sizeof(buf), "%s %s", p.ready ? "[x]" : "[ ]", p.nick);
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += 28;
  }

  snprintf(buf, sizeof(buf), "Msgs:%lu  MinHeap:%luB  Rej:%d", (unsigned long)session.msgCount(),
           (unsigned long)(session.minHeap() == 0xFFFFFFFF ? 0 : session.minHeap()), session.rejectedSockets());
  renderer.drawText(SMALL_FONT_ID, leftPad, renderer.getScreenHeight() - metrics.buttonHintsHeight - 20, buf);

  const auto labels = mappedInput.mapLabels("Stop", "Game", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ArcadeHostActivity::renderPickGame() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listH = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight - 30;

  GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, GAME_COUNT, gameSel,
               [](int i) -> std::string { return GAMES[i].name; },
               [](int i) -> std::string { return GAMES[i].packDir ? "SD pack" : "Generated"; });

  if (statusMsg[0])
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding,
                      renderer.getScreenHeight() - metrics.buttonHintsHeight - 20, statusMsg);

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ArcadeHostActivity::renderPickPack() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listH = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight - 30;

  if (packs.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, listTop + listH / 2, "No packs found");
  } else {
    GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, (int)packs.size(), packSel, [this](int i) -> std::string {
      const std::string& p = packs[i];
      size_t slash = p.rfind('/');
      return (slash != std::string::npos) ? p.substr(slash + 1) : p;
    });
  }
  if (statusMsg[0])
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding,
                      renderer.getScreenHeight() - metrics.buttonHintsHeight - 20, statusMsg);

  const auto labels = mappedInput.mapLabels("Back", "Start", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ArcadeHostActivity::renderRound() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int leftPad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + 40;
  PartyGame* g = session.activeGame();
  if (!g) return;

  renderer.drawText(UI_12_FONT_ID, leftPad, y, g->title(), true, EpdFontFamily::BOLD);
  y += 50;
  char buf[64];
  g->roundLabel(buf, sizeof(buf));
  renderer.drawText(UI_12_FONT_ID, leftPad, y, buf);
  y += 50;
  g->tallyLine(buf, sizeof(buf));
  renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);

  const auto labels = mappedInput.mapLabels("Stop", "Reveal", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ArcadeHostActivity::renderReveal() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int leftPad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + 40;
  PartyGame* g = session.activeGame();
  if (!g) return;

  renderer.drawText(UI_12_FONT_ID, leftPad, y, g->title(), true, EpdFontFamily::BOLD);
  y += 50;
  renderer.drawText(UI_12_FONT_ID, leftPad, y, "REVEAL");
  y += 50;
  char buf[64];
  g->tallyLine(buf, sizeof(buf));
  renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
  y += 40;

  // Current leader.
  int best = -1;
  for (int i = 0; i < ArcadeSession::MAX_CLIENTS; ++i) {
    if (!session.slotActive(i)) continue;
    if (best < 0 || session.playerAt(i).score > session.playerAt(best).score) best = i;
  }
  if (best >= 0) {
    snprintf(buf, sizeof(buf), "Leader: %s (%lu)", session.playerAt(best).nick,
             (unsigned long)session.playerAt(best).score);
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
  }

  const auto labels = mappedInput.mapLabels("Stop", "Next", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ArcadeHostActivity::renderPodium() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  int y = metrics.topPadding + metrics.headerHeight + 30;

  renderer.drawCenteredText(UI_12_FONT_ID, y, "PODIUM", true, EpdFontFamily::BOLD);
  y += 50;

  int idx[ArcadeSession::MAX_CLIENTS];
  int n = 0;
  for (int i = 0; i < ArcadeSession::MAX_CLIENTS; ++i)
    if (session.slotActive(i)) idx[n++] = i;
  std::sort(idx, idx + n, [this](int a, int b) { return session.playerAt(a).score > session.playerAt(b).score; });

  char buf[64];
  for (int k = 0; k < n && k < 3; ++k) {
    snprintf(buf, sizeof(buf), "%d. %s  -  %lu", k + 1, session.playerAt(idx[k]).nick,
             (unsigned long)session.playerAt(idx[k]).score);
    renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
    y += 44;
  }
  (void)pageWidth;

  const auto labels = mappedInput.mapLabels("Stop", "Lobby", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
