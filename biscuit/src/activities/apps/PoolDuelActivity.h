#pragma once
#include <DNSServer.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "duel/DuelSession.h"

// 1v1 8-ball pool host. The X4 hosts an open WiFi AP + captive redirect + the
// phone client (served gzipped from flash) + a WebSocket table on :81. Physics
// runs on the shooting phone; this device is the rules referee and relay. Two
// players are seated, everyone else spectates. Single-threaded, no BLE.
class PoolDuelActivity final : public Activity {
 public:
  explicit PoolDuelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PoolDuel", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return state == RUNNING; }

 private:
  enum State { ENTER_SSID, RUNNING, STOPPED };

  State state = ENTER_SSID;
  std::string apSsid = "POOL";
  DuelSession session;

  std::unique_ptr<WebServer> httpServer;
  std::unique_ptr<DNSServer> dnsServer;
  std::unique_ptr<WebSocketsServer> wsServer;

  void promptSsid();
  void startHost();
  void stopHost();
  void handleRoot();
  void handleNotFound();

  static void wsTrampoline(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);

  void drawJoinQr();
};
