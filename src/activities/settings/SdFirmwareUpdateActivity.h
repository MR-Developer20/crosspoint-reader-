#pragma once

#include <string>

#include "activities/Activity.h"

/**
 * SD-card based firmware update activity.
 *
 * Flow:
 *  1) onEnter -> push FileBrowserActivity in PickFirmware mode (only .bin files visible).
 *  2) On result: validate the .bin (header magic, size fits OTA partition).
 *  3) Slot selector: name the destination OTA slot and what it currently holds, and
 *     choose whether to boot into the new firmware or leave the running OS alone.
 *  4) Push ConfirmationActivity ("Update firmware?").
 *  5) On confirm: stream the file into the OTA partition via the shared firmware
 *     flasher, drawing a progress bar; then restart (or stay put, per step 3).
 *
 * The destination is always the passive slot — the running slot cannot be rewritten
 * while executing from it — so the selector chooses the boot outcome, not the write
 * target. See boot_switch (lib/BootSwitch) for the otadata mechanics.
 *
 * Used both from Settings -> System -> "SD Card Firmware Update", and as the only
 * activity launched in boot recovery mode (left side button + power on X3).
 */
class SdFirmwareUpdateActivity : public Activity {
 public:
  enum class State {
    PICKING,
    VALIDATING,
    SELECTING_SLOT,
    CONFIRMING,
    UPDATING,
    SUCCESS,
    INSTALLED,  // written to the passive slot, still running the old OS
    FAILED,
  };

  explicit SdFirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool recoveryMode = false)
      : Activity("SdFirmwareUpdate", renderer, mappedInput), recoveryMode(recoveryMode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::UPDATING || state == State::VALIDATING; }
  bool skipLoopDelay() override { return state == State::UPDATING; }

 private:
  State state = State::PICKING;
  bool recoveryMode = false;

  std::string firmwarePath;
  size_t firmwareSize = 0;
  size_t writtenBytes = 0;
  unsigned int lastRenderedPercent = 101;
  std::string errorMessage;

  // Slot selector. Labels/descriptions are captured once when the selector opens
  // so render() never touches flash. Buffers are sized for a 16-char partition
  // label and a "<label>: <32-char version>" description.
  static constexpr int slotChoiceCount = 2;
  int slotChoiceIndex = 0;  // 0 = install and boot into it, 1 = install only
  char targetSlotLabel[20] = {};
  char targetSlotDesc[64] = {};
  char runningSlotName[20] = {};
  bool bootIntoNewFirmware() const { return slotChoiceIndex == 0; }

  void launchPicker();
  void onPickerResult(const ActivityResult& result);
  bool validateFirmware();
  void showSlotSelector();
  void promptConfirmation();
  void onConfirmationResult(const ActivityResult& result);
  void startUpdate();
  void performUpdate();
};
