#include "SdFirmwareUpdateActivity.h"

#include <Arduino.h>
#include <BootSwitch.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_ota_ops.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/FirmwareFlasher.h"

void SdFirmwareUpdateActivity::onEnter() {
  Activity::onEnter();
  // Build-identity marker — confirms which firmware build owns the SD update flow.
  LOG_INF("FW", "SdFirmwareUpdateActivity build=%s %s recovery=%d", __DATE__, __TIME__, recoveryMode ? 1 : 0);
  state = State::PICKING;
  launchPicker();
}

void SdFirmwareUpdateActivity::launchPicker() {
  // Reuse the standard file browser, restricted to .bin files only.
  startActivityForResult(
      std::make_unique<FileBrowserActivity>(renderer, mappedInput, "/", FileBrowserActivity::Mode::PickFirmware),
      [this](const ActivityResult& result) { onPickerResult(result); });
}

void SdFirmwareUpdateActivity::onPickerResult(const ActivityResult& result) {
  if (result.isCancelled) {
    if (recoveryMode) {
      // Recovery mode: re-launch the picker so the user cannot escape into a half-initialised UI.
      launchPicker();
      return;
    }
    finish();
    return;
  }

  const auto* path = std::get_if<FilePathResult>(&result.data);
  if (!path) {
    LOG_ERR("FW", "Picker returned no path");
    finish();
    return;
  }
  firmwarePath = path->path;
  LOG_DBG("FW", "Selected: %s", firmwarePath.c_str());

  {
    RenderLock lock(*this);
    state = State::VALIDATING;
  }
  requestUpdateAndWait();

  if (!validateFirmware()) {
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  showSlotSelector();
}

void SdFirmwareUpdateActivity::showSlotSelector() {
  // Capture the slot facts once, here, so render() stays free of flash reads.
  // The destination is always the passive slot: the running slot cannot be
  // rewritten while executing from it.
  boot_switch::PassiveSlotInfo info = {};
  const bool targetOccupied = boot_switch::peekPassiveSlot(info);

  boot_switch::runningSlotLabel(runningSlotName, sizeof(runningSlotName));
  snprintf(targetSlotLabel, sizeof(targetSlotLabel), "%s", info.label);
  if (targetOccupied) {
    boot_switch::describeSlot(info, targetSlotDesc, sizeof(targetSlotDesc));
  } else {
    snprintf(targetSlotDesc, sizeof(targetSlotDesc), "%s", tr(STR_SLOT_EMPTY));
  }

  // Default to the familiar behaviour (boot the firmware just installed).
  slotChoiceIndex = 0;

  {
    RenderLock lock(*this);
    state = State::SELECTING_SLOT;
  }
  requestUpdate();
}

bool SdFirmwareUpdateActivity::validateFirmware() {
  HalFile file;
  if (!Storage.openFileForRead("FW", firmwarePath.c_str(), file) || !file) {
    errorMessage = tr(STR_FIRMWARE_FILE_OPEN_FAILED);
    return false;
  }
  firmwareSize = file.fileSize();
  file.close();

  // Resolve the next-update partition directly via the OTA API. Previously this
  // probed via Update.begin(firmwareSize)/Update.abort() to learn the partition
  // size, which had the side effect of erasing partition state and was wasted
  // work since we only need the size bound for validation here.
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FW", "no next-update partition available");
    errorMessage = tr(STR_INVALID_FIRMWARE);
    return false;
  }
  const size_t partitionLimit = dest->size;
  if (firmwareSize > partitionLimit) {
    LOG_ERR("FW", "firmware (%u bytes) exceeds partition (%u bytes)", static_cast<unsigned>(firmwareSize),
            static_cast<unsigned>(partitionLimit));
    errorMessage = tr(STR_FIRMWARE_TOO_LARGE);
    return false;
  }

  // Run the same end-to-end integrity check (header / segment table / XOR checksum / SHA256
  // trailer) that the shared firmware-flasher applies right before raw-writing otadata. This
  // catches truncated or corrupted .bin files at confirmation time, before the user ever sees
  // the "Updating…" progress bar.
  const auto vr = firmware_flash::validateImageFile(firmwarePath.c_str(), partitionLimit);
  if (vr != firmware_flash::Result::OK) {
    LOG_ERR("FW", "image validation failed: %s", firmware_flash::resultName(vr));
    if (vr == firmware_flash::Result::TOO_LARGE) {
      errorMessage = tr(STR_FIRMWARE_TOO_LARGE);
    } else if (vr == firmware_flash::Result::TOO_SMALL) {
      errorMessage = tr(STR_FIRMWARE_TOO_SMALL);
    } else {
      errorMessage = tr(STR_INVALID_FIRMWARE);
    }
    return false;
  }
  return true;
}

void SdFirmwareUpdateActivity::promptConfirmation() {
  {
    RenderLock lock(*this);
    state = State::CONFIRMING;
  }
  // Show "Update firmware?" with the file path as the body line.
  std::string heading = tr(STR_FIRMWARE_UPDATE_PROMPT);
  // Use the basename only to keep the body short.
  std::string body = firmwarePath;
  const auto pos = body.find_last_of('/');
  if (pos != std::string::npos) body = body.substr(pos + 1);

  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onConfirmationResult(result); });
}

void SdFirmwareUpdateActivity::onConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    // Back out to the slot selector rather than the picker: the user has
    // already chosen a file, so re-picking it would be busywork.
    RenderLock lock(*this);
    state = State::SELECTING_SLOT;
    requestUpdate();
    return;
  }

  startUpdate();
}

void SdFirmwareUpdateActivity::startUpdate() {
  {
    RenderLock lock(*this);
    state = State::UPDATING;
    writtenBytes = 0;
    lastRenderedPercent = 101;
  }
  requestUpdateAndWait();
  performUpdate();
}

void SdFirmwareUpdateActivity::performUpdate() {
  LOG_INF("FW", "SD update: %s (%u bytes)", firmwarePath.c_str(), static_cast<unsigned>(firmwareSize));

  auto progressCb = +[](size_t written, size_t total, void* ctx) {
    auto* self = static_cast<SdFirmwareUpdateActivity*>(ctx);
    self->writtenBytes = written;
    self->firmwareSize = total;
    // immediate=true: wake the render task directly. We're in a tight sync
    // loop so the main loop won't drain the requestedUpdate flag for us.
    self->requestUpdate(true);
  };

  // Re-validate at flash time (TOCTOU): SD is removable, so don't trust the
  // pre-confirmation pass. The alreadyValidated parameter on the API stays
  // for callers (e.g. an OTA staging path) where the same byte stream was
  // just hashed and there's no removable-media gap.
  const bool bootIntoNew = bootIntoNewFirmware();
  const auto result = firmware_flash::flashFromSdPath(firmwarePath.c_str(), progressCb, this,
                                                      /*alreadyValidated=*/false, /*switchBootPartition=*/bootIntoNew);
  if (result != firmware_flash::Result::OK) {
    LOG_ERR("FW", "flash failed: %s", firmware_flash::resultName(result));
    errorMessage = tr(STR_FIRMWARE_WRITE_FAILED);
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  if (!bootIntoNew) {
    // Installed into the passive slot with otadata untouched: nothing to
    // reboot into, so report and let the user carry on in the current OS.
    LOG_INF("FW", "SD firmware installed to %s, staying on %s", targetSlotLabel, runningSlotName);
    RenderLock lock(*this);
    state = State::INSTALLED;
    requestUpdate();
    return;
  }

  LOG_INF("FW", "SD firmware update complete, restarting");
  {
    RenderLock lock(*this);
    state = State::SUCCESS;
  }
  requestUpdateAndWait();
  delay(1500);
  ESP.restart();
}

void SdFirmwareUpdateActivity::loop() {
  if (state == State::SELECTING_SLOT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      {
        RenderLock lock(*this);
        slotChoiceIndex = (slotChoiceIndex + 1) % slotChoiceCount;
      }
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      promptConfirmation();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (recoveryMode) {
        state = State::PICKING;
        launchPicker();
        return;
      }
      finish();
    }
    return;
  }

  if (state == State::INSTALLED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (recoveryMode) {
        state = State::PICKING;
        launchPicker();
        return;
      }
      finish();
    }
    return;
  }

  if (state == State::FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (recoveryMode) {
        // Go back to picker so user can try a different .bin
        state = State::PICKING;
        launchPicker();
        return;
      }
      finish();
    }
  }
}

void SdFirmwareUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const char* headerText = recoveryMode ? tr(STR_RECOVERY_MODE) : tr(STR_SD_FIRMWARE_UPDATE);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerText);

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - lineHeight) / 2;

  if (state == State::VALIDATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_VALIDATING_FIRMWARE));
  } else if (state == State::UPDATING) {
    // Throttle redraws to once per percent.
    const unsigned int pct = firmwareSize > 0 ? static_cast<unsigned int>((writtenBytes * 100) / firmwareSize) : 0;
    if (pct == lastRenderedPercent) {
      return;
    }
    lastRenderedPercent = pct;

    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING), true, EpdFontFamily::BOLD);

    int y = top + lineHeight + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(pct), 100);
    y += metrics.progressBarHeight + metrics.verticalSpacing;
    // Percent label is drawn by BaseTheme::drawProgressBar; this slot is left intentionally empty
    // so the do-not-power-off line below stays at the same Y as before.
    y += lineHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF));
  } else if (state == State::SELECTING_SLOT) {
    // Slot facts, then the two outcomes. The write target is fixed (the passive
    // slot); what the user picks is whether to boot into it afterwards.
    const int rowStep = lineHeight + metrics.verticalSpacing;
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

    char line[96];
    snprintf(line, sizeof(line), "%s%s", tr(STR_SLOT_TARGET_PREFIX), targetSlotLabel);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line, true, EpdFontFamily::BOLD);
    y += rowStep;

    snprintf(line, sizeof(line), "%s%s", tr(STR_SLOT_CONTAINS_PREFIX), targetSlotDesc);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line);
    y += rowStep;

    snprintf(line, sizeof(line), "%s%s", tr(STR_SLOT_RUNNING_PREFIX), runningSlotName);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line);
    y += rowStep * 2;

    static constexpr StrId choiceIds[slotChoiceCount] = {StrId::STR_INSTALL_AND_BOOT, StrId::STR_INSTALL_ONLY};
    for (int i = 0; i < slotChoiceCount; ++i) {
      const bool selected = (i == slotChoiceIndex);
      snprintf(line, sizeof(line), "%s %s", selected ? ">" : " ", I18N.get(choiceIds[i]));
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line, true,
                        selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      y += rowStep;
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == State::SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, tr(STR_RESTARTING_HINT));
  } else if (state == State::INSTALLED) {
    char line[96];
    snprintf(line, sizeof(line), "%s%s", tr(STR_FIRMWARE_INSTALLED_TO), targetSlotLabel);
    renderer.drawCenteredText(UI_10_FONT_ID, top, line, true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing,
                              tr(STR_SWITCH_OS_TO_BOOT_HINT));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == State::FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    if (!errorMessage.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, errorMessage.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // PICKING / CONFIRMING: a sub-activity is on top, nothing to draw.
    if (recoveryMode) {
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_RECOVERY_MODE_HINT));
    }
  }

  renderer.displayBuffer();
}
