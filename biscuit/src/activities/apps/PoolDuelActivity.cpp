#include "PoolDuelActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"
#include "util/RadioManager.h"

// Phone client, gzipped into flash by scripts/build_html.py from
// src/network/html/pool.html at build time (served with Content-Encoding: gzip).
#include "network/html/poolHtml.generated.h"

namespace {
PoolDuelActivity* poolInstance = nullptr;
}

void PoolDuelActivity::onEnter() {
  Activity::onEnter();
  poolInstance = this;
  RADIO.ensureWifi();
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/pool");
  state = ENTER_SSID;
  promptSsid();
}

void PoolDuelActivity::onExit() {
  Activity::onExit();
  stopHost();
  poolInstance = nullptr;
  RADIO.shutdown();
}

void PoolDuelActivity::promptSsid() {
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

void PoolDuelActivity::startHost() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str());
  delay(100);

  dnsServer = std::make_unique<DNSServer>();
  dnsServer->start(53, "*", WiFi.softAPIP());

  httpServer = std::make_unique<WebServer>(80);
  httpServer->on("/", HTTP_GET, [this] { handleRoot(); });
  httpServer->onNotFound([this] { handleNotFound(); });
  httpServer->begin();

  wsServer = std::make_unique<WebSocketsServer>(81);
  wsServer->onEvent(PoolDuelActivity::wsTrampoline);
  wsServer->begin();

  session.begin(wsServer.get());
  state = RUNNING;
  LOG_DBG("DUEL", "AP '%s' up; free heap: %d", apSsid.c_str(), ESP.getFreeHeap());
}

void PoolDuelActivity::stopHost() {
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
}

void PoolDuelActivity::handleRoot() {
  httpServer->sendHeader("Content-Encoding", "gzip");
  httpServer->send_P(200, "text/html", poolHtml, poolHtmlCompressedSize);
}

void PoolDuelActivity::handleNotFound() {
  httpServer->sendHeader("Location", "http://192.168.4.1/", true);
  httpServer->send(302, "text/plain", "");
}

void PoolDuelActivity::wsTrampoline(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (poolInstance) poolInstance->onWsEvent(num, type, payload, length);
}

void PoolDuelActivity::onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      if (!session.onConnect(num) && wsServer) wsServer->disconnect(num);
      break;
    case WStype_DISCONNECTED:
      session.onDisconnect(num);
      break;
    case WStype_TEXT:
      session.onText(num, reinterpret_cast<const char*>(payload), length);
      break;
    case WStype_BIN:
    default:
      break;
  }
}

void PoolDuelActivity::loop() {
  if (state != RUNNING) {
    if (state == STOPPED && mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  dnsServer->processNextRequest();
  httpServer->handleClient();
  wsServer->loop();

  using B = MappedInputManager::Button;
  if (mappedInput.wasReleased(B::Back)) {
    stopHost();
    state = STOPPED;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(B::Confirm)) {
    const auto phase = session.table().phase;
    if (phase == PoolTable::Phase::WAITING)
      session.hostStart();
    else if (phase == PoolTable::Phase::GAMEOVER)
      session.hostRematch();
  }

  if (session.consumeDirty()) requestUpdate();
}

// ---------------------------------------------------------------- rendering

void PoolDuelActivity::drawJoinQr() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = metrics.topPadding + metrics.headerHeight + 10;
  const int size = 180;
  const int x = renderer.getScreenWidth() - size - metrics.contentSidePadding;
  std::string payload = "WIFI:T:nopass;S:" + apSsid + ";;";
  QrUtils::drawQrCode(renderer, Rect{x, top, size, size}, payload);
  renderer.drawText(SMALL_FONT_ID, x, top + size + 6, "Scan to join");
}

void PoolDuelActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int leftPad = metrics.contentSidePadding;

  renderer.clearScreen();
  char header[40];
  snprintf(header, sizeof(header), "8-BALL - %s", apSsid.c_str());
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

  drawJoinQr();
  const PoolTable& t = session.table();
  int y = metrics.topPadding + metrics.headerHeight + 24;
  const int lineH = 40;
  char buf[64];

  const char* p0 = session.seatNick(0);
  const char* p1 = session.seatNick(1);
  snprintf(buf, sizeof(buf), "P1: %s", p0[0] ? p0 : "(open)");
  renderer.drawText(UI_12_FONT_ID, leftPad, y, buf, true,
                    (t.turn == 0 && t.phase == PoolTable::Phase::PLAYING) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  y += lineH;
  snprintf(buf, sizeof(buf), "P2: %s", p1[0] ? p1 : "(open)");
  renderer.drawText(UI_12_FONT_ID, leftPad, y, buf, true,
                    (t.turn == 1 && t.phase == PoolTable::Phase::PLAYING) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  y += lineH + 6;

  const char* phaseStr = t.phase == PoolTable::Phase::WAITING
                             ? "Waiting for players"
                             : (t.phase == PoolTable::Phase::PLAYING ? "In play" : "Game over");
  renderer.drawText(UI_10_FONT_ID, leftPad, y, phaseStr);
  y += lineH;

  if (t.phase == PoolTable::Phase::PLAYING) {
    snprintf(buf, sizeof(buf), "Turn: %s", session.seatNick(t.turn));
    renderer.drawText(UI_10_FONT_ID, leftPad, y, buf);
    y += lineH;
    renderer.drawText(SMALL_FONT_ID, leftPad, y, t.message);
    y += lineH;
  } else if (t.phase == PoolTable::Phase::GAMEOVER && t.winner >= 0) {
    snprintf(buf, sizeof(buf), "Winner: %s", session.seatNick(t.winner));
    renderer.drawText(UI_12_FONT_ID, leftPad, y, buf, true, EpdFontFamily::BOLD);
    y += lineH;
  }

  snprintf(buf, sizeof(buf), "Spectators: %d   MinHeap: %luB", session.spectatorCount(),
           (unsigned long)(session.minHeap() == 0xFFFFFFFF ? 0 : session.minHeap()));
  renderer.drawText(SMALL_FONT_ID, leftPad, renderer.getScreenHeight() - metrics.buttonHintsHeight - 20, buf);

  const char* okLabel = "";
  if (t.phase == PoolTable::Phase::WAITING && session.seatedCount() >= 2)
    okLabel = "Start";
  else if (t.phase == PoolTable::Phase::GAMEOVER)
    okLabel = "Rematch";
  const auto labels = mappedInput.mapLabels("Stop", okLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
