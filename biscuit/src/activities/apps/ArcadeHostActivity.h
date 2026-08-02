#pragma once
#include <DNSServer.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "arcade/ArcadeSession.h"

// Whole-group WiFi party-game host. One Activity is the entire referee: it owns
// the open WiFi AP + captive redirect (mirrors CaptivePortalActivity), serves
// the phone client from flash, terminates the phone WebSockets on :81, and runs
// the game engine — all single-threaded in loop(). No BLE, no FreeRTOS tasks.
class ArcadeHostActivity final : public Activity {
 public:
  explicit ArcadeHostActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ArcadeHost", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return state == RUNNING; }

 private:
  enum State { ENTER_SSID, RUNNING, STOPPED };
  // Host sub-view while the session is in its LOBBY phase.
  enum HostView { HV_LOBBY, HV_PICK_GAME, HV_PICK_PACK };

  State state = ENTER_SSID;
  HostView hostView = HV_PICK_GAME;

  std::string apSsid = "ARCADE";
  ArcadeSession session;

  std::unique_ptr<WebServer> httpServer;
  std::unique_ptr<DNSServer> dnsServer;
  std::unique_ptr<WebSocketsServer> wsServer;

  int gameSel = 0;
  int packSel = 0;
  int selectedGameIdx = -1;
  std::vector<std::string> packs;
  char statusMsg[48] = {0};

  // Bring-up / teardown
  void startHost();
  void stopHost();
  void handleRoot();
  void handleNotFound();

  // Host flow
  void promptSsid();
  void onGameChosen(int idx);
  void onPackChosen();

  // WebSocket trampoline (C-style static → instance, mirroring CrossPointWebServer)
  static void wsTrampoline(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);

  // Rendering helpers
  void renderLobby();
  void renderPickGame();
  void renderPickPack();
  void renderRound();
  void renderReveal();
  void renderPodium();
  void drawJoinQr();
};
