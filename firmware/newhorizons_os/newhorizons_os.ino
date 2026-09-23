#include <Arduino.h>
#include <Wire.h>
#include <esp_now.h>

#include "BoardPins.h"
#include "ActionButtonPolicy.h"
#include "AirtimeArbiter.h"
#include "AppManager.h"
#include "BootModeManager.h"
#include "Calibration.h"
#include "Config.h"
#include "ControlServer.h"
#include "DeviceConfig.h"
#include "DisplayManager.h"
#include "EspNowOtaReceiver.h"
#include "EspNowPairing.h"
#include "EspNowStreamTransport.h"
#include "ExternalLedController.h"
#include "FaultRecorder.h"
#include "FindMeClient.h"
#include "ImuManager.h"
#include "MagnetometerManager.h"
#include "BatteryGaugeManager.h"
#include "LedController.h"
#include "MatrixScanner.h"
#include "OtaManager.h"
#include "PacketBuilder.h"
#include "PowerAnimation.h"
#include "PowerGovernor.h"
#include "AppGovernor.h"
#include "AppRegistry.h"
#include "FlowApp.h"
#include "PowerManager.h"
#include "PowerStateManager.h"
#include "ProcFs.h"
#include "Scheduler.h"
#include "ServiceManager.h"
#include "StreamTransport.h"
#include "Storage.h"
#include "TimeSync.h"
#include "UdpStreamTransport.h"
#include "Watchdog.h"
#include "WifiManager.h"

// Takes the OTA rollback decision away from the Arduino core. The core's
// initArduino() would otherwise call esp_ota_mark_app_valid_cancel_rollback()
// before setup() even runs (cores/esp32/esp32-hal-misc.c), which confirms a
// freshly-OTA'd image before a single line of NHOS has executed -- so a
// firmware that panics during boot would be kept, not reverted. Returning
// true defers the decision to BootModeManager::confirmFirmwareValid(), called
// at the end of a boot that actually reached runtime_ready.
//
// Consequence to keep in mind when editing setup(): anything that resets the
// device before that call now costs the update, not just the boot.
extern "C" bool verifyRollbackLater() {
  return true;
}

namespace {

nhos::Storage storage;
nhos::DeviceConfig deviceConfig;
nhos::BootModeManager bootMode;
nhos::FaultRecorder faults;
nhos::Scheduler scheduler;
nhos::ProcFs procFs;
nhos::ServiceManager services;
nhos::AppManager apps;
nhos::AppGovernor appGovernor;
nhos::AppRegistry appRegistry;
nhos::FlowApp flowApps[nhos::AppRegistry::kMaxSlots];
nhos::FlowApp& flowApp = flowApps[0];  // slot 0, the target of app_load_flow
// The last scanned frame, kept at file scope so the apps task can read it
// after scan_stream has returned. Single-threaded, so this is a handover, not
// sharing -- and it is the same buffer the scanner filled, not a copy.
nhos::MatrixFrame lastFrame;
bool lastFrameReady = false;
float lastImuSample[nhos::kImuSampleFloats] = {0};
bool lastImuValid = false;
nhos::LedController leds;
nhos::ExternalLedController externalLeds;
nhos::DisplayManager displayManager;
nhos::WifiManager wifi;
nhos::MatrixScanner scanner;
nhos::Calibration calibration;
nhos::PacketBuilder packetBuilder;
nhos::PowerManager power;
nhos::PowerStateManager powerState;
nhos::PowerGovernor powerGovernor;
nhos::ImuManager imu;
nhos::BatteryGaugeManager batteryGauge;
nhos::MagnetometerManager magnetometer;
nhos::FindMeClient findme;
nhos::ControlServer control;
nhos::OtaManager ota;
nhos::TimeSync timeSync;
// Single WiFiUDP for the whole wifi_udp path lives inside udpTransport.
// Do not add a second WiFiUDP bound to kUdpStreamPort: dual-bind steals
// inbound command packets from serviceUdpCommand() while outbound stream
// still appears healthy (Gateway last_seen updates, commands time out).
nhos::UdpStreamTransport udpTransport;
nhos::EspNowStreamTransport espNowTransport;
nhos::EspNowPairing espNowPairing;
nhos::EspNowOtaReceiver espNowOtaReceiver;
nhos::AirtimeArbiter airtime;
nhos::StreamTransport* activeTransport = nullptr;
bool espNowMode = false;

void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  espNowPairing.handleEspNowRecv(info->src_addr, data, static_cast<size_t>(len));
}

// Diagnostic only (temporary): esp_now_send()'s own synchronous return
// value only confirms a packet was accepted into the driver's TX queue,
// NOT that it was actually transmitted/acked over the air -- this project
// never previously registered a send-status callback anywhere, so a
// send that's silently dropped at the RF/driver layer after being queued
// successfully was invisible. Logs failures only, to isolate whether
// control-response fragment loss (see EspNowPairing.cpp's serviceCommand())
// is happening here vs. somewhere else.
void onEspNowSent(const esp_now_send_info_t* /*info*/, esp_now_send_status_t status) {
  espNowPairing.handleSendStatus(status == ESP_NOW_SEND_SUCCESS);
}

// Airtime claimant predicates. Both read state the owning module already
// derives, so the arbiter can poll instead of relying on paired
// acquire/release calls that could leak a claim on an error path.
bool otaRelayNeedsAirtime(void*) { return espNowOtaReceiver.isRelaying(); }
bool commandResponseNeedsAirtime(void*) { return espNowPairing.hasPendingCommandWork(); }

uint8_t packetBuffer[
    nhos::kMaxPacketBytes
];

uint32_t heartbeatSeq = 1;
uint32_t lastHeartbeatAttemptMs = 0;
bool criticalError = false;
uint32_t lastObservedOverrunFrames = 0;
uint32_t lastObservedUdpFailures = 0;
bool runtimeServicesSuspended = false;
bool softOffOutputsSleeping = false;

void logBoot(const String& message) {
  Serial.println(message);
  storage.logLine(String(millis()) + " " + message);
}

void logPowerEvent(const String& message) {
  Serial.println(message);
}

void serviceAutoOta(bool wifiConnected) {
  if (!wifiConnected || !deviceConfig.data().ota.autoApplyOnBoot) {
    return;
  }
  // Never chain an update off an image that is still on probation: this
  // runs before confirmFirmwareValid(), so the ESP.restart() below would
  // race the bootloader's revert-on-reset for the running slot. Deferring
  // to the next boot costs nothing -- by then the image is confirmed.
  if (bootMode.otaPendingVerify()) {
    logBoot("auto_ota_deferred ota_pending_verify=true");
    return;
  }
  logBoot("auto_ota_enabled");
  leds.setSignal(nhos::LedSignal::OtaActive);
  leds.service(millis());
  const bool applied = ota.autoApplyIfNewer(deviceConfig.data().ota.manifestUrl);
  if (!applied) {
    if (ota.lastPhase() == "current") {
      return;
    }
    logBoot(String("auto_ota_apply_failed status=") + ota.lastStatusJson());
    // Keep OTA failure in serial/status, but do not overwrite the base
    // Gateway/Hub discovery indication with a red burst during every boot.
    // Explicit OTA commands still report their result through ControlServer.
    return;
  }
  leds.showEvent(nhos::LedSignal::OtaSuccess);
  leds.service(millis());
  delay(100);
  ESP.restart();
}

String chargeStateName() {
  switch (power.chargeState()) {
    case nhos::ChargeState::ChargingOrMissing:
      return "charging";
    case nhos::ChargeState::ChargeDone:
      return "done";
    case nhos::ChargeState::NotCharging:
    default:
      return "idle";
  }
}

void suspendRuntimeServicesForSoftOff() {
  if (!runtimeServicesSuspended) {
    scanner.stop();
    wifi.suspend();
    imu.setEnabled(false);
    // Frequency is the governor's call now; it drops to the floor as soon as
    // runtimeActive goes false on the next tick.
    // Soft-off light-sleeps for kSoftOffBatterySleepUs == 5s, exactly the
    // task WDT timeout -- feeding around the sleep would be a coin flip.
    // Disarming is also the honest semantics: the watchdog exists to catch a
    // wedged runtime loop, and in soft-off there is deliberately no runtime.
    nhos::watchdogDisarm();
    runtimeServicesSuspended = true;
    logPowerEvent("runtime_services_suspended");
  }
}

void applySoftOffIndicators() {
  if (powerState.state() == nhos::PowerState::SoftOffCharging && power.chargeState() == nhos::ChargeState::NotCharging) {
    leds.setSignal(nhos::LedSignal::Off);
    leds.showEvent(nhos::LedSignal::SoftOffChargeIdle);
  }
  leds.service(millis());
}

void applyNormalRuntimeState() {
  if (softOffOutputsSleeping) {
    externalLeds.wake();
    displayManager.wake();
    softOffOutputsSleeping = false;
  }
  if (!runtimeServicesSuspended) {
    return;
  }
  // Governor restores the clock; going through it keeps currentMhz_ honest.
  powerGovernor.service(scanner.active(), true);
  nhos::watchdogArm();
  wifi.resume();
  imu.setEnabled(deviceConfig.data().imuEnabled);
  if (bootMode.mode() == nhos::RunMode::Normal && scanner.hasLayout() && !scanner.active()) {
    scanner.start();
  }
  runtimeServicesSuspended = false;
  logPowerEvent("runtime_services_resumed");
}

void servicePowerTransition() {
  switch (powerState.transitionPhase()) {
    case nhos::PowerTransitionPhase::ShutdownAnimationPending:
      suspendRuntimeServicesForSoftOff();
      externalLeds.sleep();
      displayManager.sleep();
      softOffOutputsSleeping = true;
      powerState.finishPowerTransition();
      logPowerEvent("shutdown_done");
      break;
    case nhos::PowerTransitionPhase::ShutdownAnimationRunning:
    case nhos::PowerTransitionPhase::WakeAnimationRunning:
      powerState.finishPowerTransition();
      break;
    case nhos::PowerTransitionPhase::None:
    default:
      break;
  }
}

void servicePowerState() {
  const nhos::ActionButtonGesture gesture = powerState.service(
      millis(), power.chargerDetected(), power.chargeState());
#if NHOS_BOARD_HAS_BUTTON
  // Apps see short presses on every board with a button, beside whatever the
  // press is configured to do. A long press never reaches them: it is the
  // soft-off gesture, and an app must not be able to make it mean anything
  // else.
  if (gesture == nhos::ActionButtonGesture::ShortPress) {
    nhos::AppEvent press;
    press.kind = nhos::AppEventKind::Button;
    press.nowMs = millis();
    press.frameSeq = lastFrame.seq;
    apps.dispatch(press);
  }
#endif
#if defined(NHOS_BOARD_V15F)
  if (gesture != nhos::ActionButtonGesture::None) {
    const String actionName = gesture == nhos::ActionButtonGesture::ShortPress
        ? deviceConfig.data().actionButton.shortPress
        : deviceConfig.data().actionButton.longPress;
    const nhos::ActionButtonAction action = nhos::actionButtonActionFromName(actionName.c_str());
    bool succeeded = false;
    switch (action) {
      case nhos::ActionButtonAction::None:
        succeeded = true;
        break;
      case nhos::ActionButtonAction::SoftOff:
        powerState.requestState(
            power.chargerDetected() ? nhos::PowerState::SoftOffCharging : nhos::PowerState::SoftOffBattery,
            "action_button_configured_soft_off");
        succeeded = true;
        break;
      case nhos::ActionButtonAction::Identify:
        leds.showEvent(nhos::LedSignal::ActionButtonIdentify);
        externalLeds.identify();
        succeeded = true;
        break;
      case nhos::ActionButtonAction::ToggleExternalLed: {
        const nhos::ExternalLedConfig previous = deviceConfig.data().externalLed;
        const String nextMode = previous.mode == "enabled" ? "off" : "enabled";
        if (deviceConfig.setExternalLed(nextMode, previous.preset, previous.brightness, previous.color) &&
            deviceConfig.save(storage)) {
          externalLeds.apply(deviceConfig.data().externalLed);
          leds.showEvent(nhos::LedSignal::CommandSuccess);
          succeeded = true;
        } else {
          deviceConfig.setExternalLed(previous.mode, previous.preset, previous.brightness, previous.color);
          leds.showEvent(nhos::LedSignal::CommandFailed);
        }
        break;
      }
      case nhos::ActionButtonAction::Invalid:
      default:
        leds.showEvent(nhos::LedSignal::CommandFailed);
        break;
    }
    powerState.setLastAction(action, succeeded);
  }
#elif NHOS_BOARD_HAS_BUTTON
  if (gesture == nhos::ActionButtonGesture::LongPress) {
    powerState.requestState(
        power.chargerDetected() ? nhos::PowerState::SoftOffCharging : nhos::PowerState::SoftOffBattery,
        "action_button_long_press");
  }
#endif
  if (!powerState.consumeTransition()) {
    return;
  }
  if (powerState.shouldRunServices()) {
    if (softOffOutputsSleeping) {
      externalLeds.wake();
      displayManager.wake();
      softOffOutputsSleeping = false;
    }
    applyNormalRuntimeState();
  } else {
    suspendRuntimeServicesForSoftOff();
  }
}

// WiFi/UDP mode gates streaming on wifi.isConnected(); ESP-NOW mode has no
// AP association to wait for at all (readiness -- i.e. "paired with a
// Hub" -- is checked per-send by StreamTransport::ready(), not here).
bool streamingGateOk() {
  if (control.maintenanceMode()) {
    return false;
  }
  // Sensor streaming yields to anything higher priority on the radio: an
  // OTA relay transferring chunks, or a control response being paced out.
  // Both used to be separate hardcoded checks here, each discovered the
  // hard way on real hardware (starved command responses, stalled relays);
  // they are now the bottom of AirtimeArbiter's priority table.
  //
  // MatrixScanner's ring buffer just drops the oldest queued frames while
  // paused (sensor data is lossy by design already) and resumes normally --
  // no special draining/resume logic needed here.
  if (espNowMode && !airtime.canClaim(nhos::AirtimeClass::SensorStream)) {
    return false;
  }
  return espNowMode || wifi.isConnected();
}

// kAppCapDriveLed was declared in v1.0.0 but had nothing behind it, so an app
// could ask for the LED and then discover there was no way to use it. This is
// that way. AppManager re-checks the capability before calling through.
void applyAppLed(uint8_t r, uint8_t g, uint8_t b) {
  leds.setStatus(nhos::LedColor{r, g, b});
}

bool appDisplayLine(uint8_t row, nhos::AppDisplayLine& out) {
  return apps.displayLine(row, out);
}

// Runs after scan_stream in the same tick, so the frame it dispatches is the
// one just scanned and is still the scanner's own data.
void taskApps() {
  const uint32_t nowMs = millis();
  appGovernor.update(scanner.health(), nowMs);

  if (lastFrameReady) {
    lastFrameReady = false;
    nhos::AppEvent event;
    event.kind = nhos::AppEventKind::Frame;
    event.nowMs = nowMs;
    event.frameSeq = lastFrame.seq;
    event.frame = &lastFrame;
    event.imuSample = lastImuValid ? lastImuSample : nullptr;
    apps.dispatch(event);
  }

  static uint32_t lastTickMs = 0;
  if (nowMs - lastTickMs >= 100) {
    lastTickMs = nowMs;
    nhos::AppEvent tick;
    tick.kind = nhos::AppEventKind::Tick;
    tick.nowMs = nowMs;
    tick.frameSeq = lastFrame.seq;
    apps.dispatch(tick);
  }
}

void scanAndStreamIfDue() {
  if (!streamingGateOk()) {
    return;
  }
  if (!scanner.scanDue()) {
    return;
  }

  nhos::MatrixFrame frame;
  const size_t matrixPayloadLen = scanner.scanIntoPacketPayload(
      packetBuffer + nhos::kPacketHeaderLen,
      sizeof(packetBuffer) - nhos::kPacketHeaderLen,
      frame);
  if (!matrixPayloadLen || !scanner.shouldSendFrame(frame)) {
    return;
  }

  float imuSample[nhos::kImuSampleFloats] = {0};
  const bool imuSampleValid = imu.copyLatestSample(imuSample);
  float magSample[nhos::kPacketMagFloatCount] = {0};
  const bool magSampleValid = magnetometer.copyLatestSample(magSample);
  nhos::BatteryGaugeSample gaugeSample;
  nhos::BatterySample batterySample;
  const bool batterySampleValid = batteryGauge.copyLatestSample(gaugeSample);
  if (batterySampleValid) {
    batterySample.status = gaugeSample.status;
    batterySample.fault = gaugeSample.fault;
    batterySample.vbatMv = gaugeSample.vbatMv;
    batterySample.socCentiPercent = gaugeSample.socCentiPercent;
  }
  // Handed to the apps task rather than dispatched here. Apps are no longer
  // passengers on the scan: they have their own scheduler entry, so an app
  // that only wants a periodic tick keeps running when scanning stops, and
  // the scan's cost stays attributable to the scan.
  lastFrame = frame;
  lastImuValid = imuSampleValid;
  if (imuSampleValid) {
    memcpy(lastImuSample, imuSample, sizeof(lastImuSample));
  }
  lastFrameReady = true;

  const bool epochValid = timeSync.hasSynced();
  const uint64_t epochMs = epochValid ? timeSync.nowEpochMs() : 0;
  size_t len = packetBuilder.buildMatrixPacketHeader(
      frame, epochMs, epochValid, packetBuffer, sizeof(packetBuffer),
      matrixPayloadLen, imuSampleValid ? imuSample : nullptr,
      magSampleValid ? magSample : nullptr,
      batterySampleValid ? &batterySample : nullptr);
  if (!len) {
    scanner.recordUdpSend(false, 0);
    return;
  }
  if (scanner.streamBufferEnabled()) {
    if (!scanner.enqueuePacket(packetBuffer, len, frame.seq, frame.timestampMs, packetBuffer[3])) {
      scanner.recordUdpSend(false, 0);
    }
    return;
  }
  // Matches the original UDP-only code's behavior of not touching
  // recordUdpSend() at all when there's nowhere to send yet (no
  // discovered/configured host in wifi_udp mode; not yet paired in
  // espnow mode) -- see StreamTransport::ready()'s doc comment.
  if (!activeTransport->ready()) {
    return;
  }
  const uint32_t udpStartUs = micros();
  const bool sent = activeTransport->sendFrame(packetBuffer, len);
  scanner.recordUdpSend(sent, micros() - udpStartUs);
}

void sendQueuedPacketIfAny() {
  if (!streamingGateOk() || !scanner.streamBufferEnabled()) {
    return;
  }
  scanner.sendQueuedPacket(*activeTransport);
}

void sendHeartbeatIfDue() {
  if (!wifi.isConnected() || !findme.hasGateway()) {
    return;
  }
  uint32_t now = millis();
  if (lastHeartbeatAttemptMs && now - lastHeartbeatAttemptMs < nhos::kHeartbeatIntervalMs) {
    return;
  }
  lastHeartbeatAttemptMs = now;
  const bool epochValid = timeSync.hasSynced();
  const uint64_t epochMs = epochValid ? timeSync.nowEpochMs() : 0;
  size_t len = packetBuilder.buildHeartbeat(heartbeatSeq++, epochMs, epochValid, packetBuffer, sizeof(packetBuffer));
  if (!len) {
    findme.recordHeartbeat(now, "heartbeat_encode_failed");
    return;
  }
  // Share the single bound UDP socket with stream + command RX (udpTransport).
  if (!activeTransport || !activeTransport->ready()) {
    findme.recordHeartbeat(now, "heartbeat_not_ready");
    return;
  }
  if (activeTransport->sendFrame(packetBuffer, len)) {
    findme.recordHeartbeat(now, "");
  } else {
    findme.recordHeartbeat(now, "heartbeat_send_failed");
  }
}

// ---------------------------------------------------------------------------
// Task-table entry points.
//
// Each wraps exactly one step of the pre-scheduler loop(). They exist because
// the table stores plain void(*)() and several of the underlying service()
// calls take a timestamp argument. Nothing here changes what runs or when --
// see registerRuntimeTasks() for how the original ordering is preserved.
// ---------------------------------------------------------------------------
void taskPower() { power.service(millis()); }
void taskBatteryGauge() { batteryGauge.service(millis()); }
void taskImu() { imu.service(micros()); }
void taskMagnetometer() { magnetometer.service(millis()); }

void taskEspNow() {
  espNowPairing.service();
  espNowOtaReceiver.service();
  // Before the stream transport, so a higher-priority paced burst keeps
  // making progress rather than queueing behind sensor frames.
  airtime.service();
  espNowTransport.service();
}

void taskWifi() { wifi.service(); }

void taskFindMe() {
  findme.setModeName(bootMode.modeName());
  findme.service();
}

void taskControl() { control.service(); }
void taskControlUdp() { control.serviceUdpCommand(udpTransport.udp()); }

void taskTimeAndHeartbeat() {
  if (wifi.isConnected()) {
    timeSync.begin();  // no-op after first successful call; covers WiFi connecting after boot
  }
  sendHeartbeatIfDue();
}

void updateLedState() {
  const uint32_t nowMs = millis();
  nhos::BatteryGaugeSample batterySample;
  const bool batterySampleValid = batteryGauge.copyLatestSample(batterySample);
  leds.setBatteryStatus(
      batterySampleValid, batterySample.socCentiPercent,
      power.chargeState() == nhos::ChargeState::ChargingOrMissing,
      deviceConfig.data().batteryLed.lowBatteryThresholdPercent);
  const nhos::ScanHealth health = scanner.health();
  nhos::ExternalLedInputs extIn;
  extIn.wifiConnected = wifi.isConnected();
  extIn.wifiBusy = wifi.setupActive();
  extIn.hasGateway = findme.hasGateway();
  extIn.pressure01 = scanner.lastPeak01();
  extIn.calibrating = calibration.sessionActive();
  apps.extLedFrame(extIn.app);
  const bool transportAttached =
      espNowMode ? espNowPairing.hasHub() : findme.hasGateway();
  // Treat failures observed while finding a Gateway/Hub as startup history,
  // not an operator-facing scan warning. The base signal must remain the
  // blue/orange search breathe until a fresh attachment exists.
  if (!transportAttached) {
    lastObservedOverrunFrames = health.overrunFrames;
    lastObservedUdpFailures = health.udpSendFailures;
  } else if (health.overrunFrames > lastObservedOverrunFrames ||
             health.udpSendFailures > lastObservedUdpFailures) {
    leds.showEvent(nhos::LedSignal::ScanWarning);
    lastObservedOverrunFrames = health.overrunFrames;
    lastObservedUdpFailures = health.udpSendFailures;
  }

  if (powerState.state() == nhos::PowerState::SoftOffBattery) {
    leds.setSignal(nhos::LedSignal::Off);
    leds.service(nowMs);
    extIn.systemSignal = nhos::LedSignal::Off;
    externalLeds.service(nowMs, health, extIn);
    return;
  }
  if (powerState.state() == nhos::PowerState::SoftOffCharging) {
#if defined(NHOS_BOARD_V15F)
    // v1.5.F has a dedicated battery WS2812B. Keep the system pixel reserved
    // for connectivity even when the legacy soft-off path is exercised.
    leds.setSignal(nhos::LedSignal::Off);
#else
    if (power.chargeState() == nhos::ChargeState::ChargingOrMissing) {
      leds.setSignal(nhos::LedSignal::SoftOffCharging);
    } else if (power.chargeState() == nhos::ChargeState::ChargeDone) {
      leds.setSignal(nhos::LedSignal::SoftOffChargeDone);
    } else {
      leds.setSignal(nhos::LedSignal::Off);
    }
#endif
    leds.service(nowMs);
    extIn.systemSignal = nhos::LedSignal::Off;
    externalLeds.service(nowMs, health, extIn);
    return;
  }
  nhos::LedSignal activeSignal = nhos::LedSignal::Online;
  if (criticalError) {
    activeSignal = nhos::LedSignal::Error;
  } else if (ESP.getFreeHeap() < 30000 || ESP.getMaxAllocHeap() < 12000) {
    activeSignal = nhos::LedSignal::RamDanger;
  } else if (espNowMode) {
    // Mirrors streamingGateOk()'s own espNowMode-first special-casing --
    // wifi.isConnected()/setupActive()/findme.hasGateway() are all
    // permanently false in this mode (WiFi is never brought up), so
    // without this branch every ESP-NOW device would fall through to
    // WifiConnecting and stay there even once genuinely paired. Flat
    // paired-vs-not check, deliberately not nested with the charge-state
    // branches below -- a paired ESP-NOW device shows solid Online
    // regardless of charge state; see EspNowConnecting's own comment in
    // LedController.h for why this is a distinct signal from
    // WifiConnecting rather than reusing it.
    activeSignal = espNowPairing.hasHub() ? nhos::LedSignal::Online : nhos::LedSignal::EspNowConnecting;
  } else if (wifi.setupActive()) {
    activeSignal = nhos::LedSignal::WifiSetup;
  } else if (!wifi.isConnected()) {
    activeSignal = nhos::LedSignal::WifiConnecting;
  } else if (!findme.hasGateway()) {
    activeSignal = nhos::LedSignal::FindMePending;
  } else if (bootMode.mode() == nhos::RunMode::SafeMaintenance) {
    activeSignal = nhos::LedSignal::SafeMode;
  } else if (control.maintenanceMode()) {
    activeSignal = nhos::LedSignal::Maintenance;
#if !defined(NHOS_BOARD_V15F)
  } else if (power.chargeState() == nhos::ChargeState::ChargeDone) {
    activeSignal = nhos::LedSignal::ChargeDone;
  } else if (power.chargeState() == nhos::ChargeState::ChargingOrMissing) {
    activeSignal = nhos::LedSignal::ChargingOrMissing;
#endif
  } else {
    activeSignal = nhos::LedSignal::Online;
  }
  leds.setSignal(activeSignal);
  leds.service(nowMs);
  extIn.systemSignal = activeSignal;
  externalLeds.service(nowMs, health, extIn);
}

void taskDisplay() {
  const uint32_t nowMs = millis();
  if (!displayManager.refreshDue(nowMs)) {
    return;
  }
  displayManager.service(
      nowMs,
      wifi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString(),
      findme.hasGateway() ? findme.streamHost() : String("-"),
      scanner.health(),
      ESP.getFreeHeap(),
      ESP.getHeapSize());
}

// The old loop()'s `else` branch. Registered as an alwaysRun task placed
// after every gated one, so it still only executes when the runtime is
// parked and still runs last -- same position, same condition.
void taskSoftOff() {
  if (powerState.shouldRunServices()) {
    return;
  }
  if (powerState.transitionPhase() != nhos::PowerTransitionPhase::None) {
    servicePowerTransition();
    return;
  }
  applySoftOffIndicators();
  updateLedState();
  // powerState.lightSleep() emits:
  // soft_off_sleep_enter state=
  // soft_off_wake cause=
  powerState.lightSleep();
}

bool runtimeGate() {
  return powerState.shouldRunServices();
}

void taskServices() { services.service(millis()); }

void taskPowerGovernor() {
  powerGovernor.service(scanner.active(), powerState.shouldRunServices());
}

// ---------------------------------------------------------------------------
// Service supervision hooks.
//
// Each restartable module gets a start function that re-runs its begin() and
// reports whether the hardware actually came back, plus a health probe that
// the supervisor polls once a second. A sensor whose I2C bus wedges now
// recovers on its own instead of staying disabled for the rest of the boot.
// ---------------------------------------------------------------------------
bool imuHealthy(void*) {
  // A deliberately disabled IMU is healthy, not broken.
  return !deviceConfig.data().imuEnabled || imu.initialized();
}

bool imuStart(void*) {
  imu.begin(deviceConfig.data().imuEnabled);
  imu.setServiceIntervalUs(scanner.scanIntervalUs());
  return imuHealthy(nullptr);
}

#if NHOS_BOARD_HAS_MAG
bool magnetometerHealthy(void*) { return magnetometer.initialized(); }

bool magnetometerStart(void*) {
  // Follows the IMU's actual init result, not a board flag -- BMM150 is
  // hosted by the combined IMU driver.
  magnetometer.begin(imu.initialized());
  return magnetometer.initialized();
}
#endif

#if NHOS_BOARD_HAS_MAX17048
bool batteryGaugeHealthy(void*) {
  nhos::BatteryGaugeSample sample;
  return batteryGauge.copyLatestSample(sample);
}

bool batteryGaugeStart(void*) {
  batteryGauge.begin();
  batteryGauge.service(millis());
  return batteryGaugeHealthy(nullptr);
}
#endif

bool scannerHealthy(void*) {
  // No layout configured is a configuration state, not a fault; and the
  // scanner is deliberately stopped in maintenance mode.
  if (!scanner.hasLayout() || bootMode.mode() != nhos::RunMode::Normal) {
    return true;
  }
  return scanner.active();
}

bool scannerStart(void*) {
  if (!scanner.hasLayout() || bootMode.mode() != nhos::RunMode::Normal) {
    return true;
  }
  return scanner.start();
}

void registerServices() {
  // Foundational services: reported, never auto-restarted. Re-running their
  // begin() mid-flight would be more dangerous than the fault it is meant to
  // recover from.
  services.registerService("storage", true);
  services.registerService("config", true);
  services.registerService("leds", true);
  services.registerService("power", true);
  services.registerService("control", true);

  services.registerService("scanner", scanner.hasLayout(), &scannerStart, &scannerHealthy);
  services.registerService("imu", imuHealthy(nullptr), &imuStart, &imuHealthy);
#if NHOS_BOARD_HAS_MAG
  services.registerService("magnetometer", magnetometer.initialized(), &magnetometerStart,
                           &magnetometerHealthy);
#endif
#if NHOS_BOARD_HAS_MAX17048
  services.registerService("battery_gauge", batteryGaugeHealthy(nullptr), &batteryGaugeStart,
                           &batteryGaugeHealthy);
#endif
  services.registerService("transport", true);
  // No start hook: in Direct mode the clock can only be set from outside
  // (set_time), and under WiFi ESP-IDF's SNTP client re-syncs on its own.
  services.registerService("clock", true);
  services.registerService("apps", true);
}

// Registration order IS the dispatch order, and it reproduces the call
// sequence the hand-written loop() used. The espNowMode branch is resolved
// once here rather than re-tested every iteration: transport mode is fixed
// at boot (set_transport only takes effect on the next one), so the two
// modes simply register different tables.
void registerRuntimeTasks() {
  scheduler.setRuntimeGate(&runtimeGate);
  scheduler.registerTask("power", &taskPower, true);
  scheduler.registerTask("battery_gauge", &taskBatteryGauge, true);
  scheduler.registerTask("power_state", &servicePowerState, true);
  scheduler.registerTask("services", &taskServices, true);
  // alwaysRun and placed after power_state so it sees this tick's decision,
  // including the soft-off transition.
  scheduler.registerTask("governor", &taskPowerGovernor, true);

  scheduler.registerTask("imu", &taskImu, false);
  scheduler.registerTask("magnetometer", &taskMagnetometer, false);
  scheduler.registerTask("scan_stream", &scanAndStreamIfDue, false);
  scheduler.registerTask("stream_queue", &sendQueuedPacketIfAny, false);
  // MUST follow scan_stream: taskApps reads the frame it just produced.
  scheduler.registerTask("apps", &taskApps, false);
  if (espNowMode) {
    scheduler.registerTask("espnow", &taskEspNow, false);
  } else {
    scheduler.registerTask("wifi", &taskWifi, false);
    scheduler.registerTask("findme", &taskFindMe, false);
  }
  scheduler.registerTask("control", &taskControl, false);
  if (!espNowMode) {
    scheduler.registerTask("control_udp", &taskControlUdp, false);
    scheduler.registerTask("time_heartbeat", &taskTimeAndHeartbeat, false);
  }
  // 100Hz, not every loop pass. The loop spins tens of thousands of times a
  // second, and each pass here reads the heap's largest free block (a walk
  // under a lock) and re-renders every pattern; no LED animation changes
  // faster than 10ms, so the rest of those passes produced nothing.
  scheduler.registerTask("led", &updateLedState, false, 10000);
  scheduler.registerTask("power_transition", &servicePowerTransition, false);
  scheduler.registerTask("display", &taskDisplay, false);

  scheduler.registerTask("soft_off", &taskSoftOff, true);
}

}  // namespace

void setup() {
  Serial.begin(115200);
#if defined(NHOS_BOARD_V15F) && ARDUINO_USB_MODE
  // Native USB must never stall the real-time loop when the host has no
  // serial reader. Logs remain best-effort and resume when a reader opens.
  Serial.setTxTimeoutMs(0);
#endif
  delay(100);
  Serial.println();
  Serial.println("New Horizons OS Arduino boot");

  storage.begin();
  logBoot("boot_stage=storage_ready");
  deviceConfig.load(storage);
  storage.configureLog(
      deviceConfig.data().logging.enabled,
      deviceConfig.data().logging.maxBytes,
      deviceConfig.data().logging.level);
  logBoot(String("boot_stage=config_ready ") + deviceConfig.statusJson());
  leds.begin();
  leds.setBrightness(deviceConfig.data().boardLed.brightness);
  externalLeds.begin(deviceConfig.data().externalLed);
  logBoot("boot_stage=leds_ready");
  faults.begin();
  logBoot(String("boot_stage=fault_recorder_ready ") + faults.statusJson());
  if (faults.lastBootCrashed()) {
    logBoot(String("previous_boot_crashed ") + faults.historyJson());
  }
  bootMode.begin();
  logBoot(String("boot_stage=boot_mode_ready mode=") + bootMode.modeName() +
          " ota_rollback=" + bootMode.otaRollbackStatusJson());
  if (bootMode.rolledBackFrom().length() > 0) {
    logBoot(String("ota_rolled_back_from=") + bootMode.rolledBackFrom());
  }
  powerState.begin();
  logBoot(String("boot_stage=power_state_ready ") + powerState.statusJson());
  Wire.begin(nhos::kI2cSda, nhos::kI2cScl, NHOS_BOARD_I2C_HZ);
  logBoot(String("boot_stage=i2c_ready sda=") + String(nhos::kI2cSda) + " scl=" + String(nhos::kI2cScl));
  displayManager.setAppLineSource(&appDisplayLine);
  displayManager.begin(deviceConfig.data().oled);
  logBoot(String("boot_stage=display_ready ") + displayManager.statusJson());
  powerGovernor.begin(nhos::PowerGovernor::profileFromName(
      storage.getString("power_profile", "performance").c_str()));
  logBoot(String("boot_stage=power_governor_ready ") + powerGovernor.statusJson());
  power.begin(storage.getString("charge_profile", "slow"));
  logBoot(String("boot_stage=power_ready ") + power.statusJson());
#if NHOS_BOARD_HAS_MAX17048
  batteryGauge.setPowerManager(power);
#endif
  batteryGauge.begin();
  const uint32_t storedBatteryCapacityMah =
      storage.getUInt("battery_profile_capacity_mah", 0);
  const uint32_t storedBatteryMaxCurrentMa =
      storage.getUInt("battery_profile_max_charge_current_ma", 0);
  if (storedBatteryCapacityMah <= 65535 && storedBatteryMaxCurrentMa <= 65535) {
    const nhos::ManualBatteryProfile storedManualProfile{
        static_cast<uint16_t>(storedBatteryCapacityMah),
        static_cast<uint16_t>(storedBatteryMaxCurrentMa), true};
    if (nhos::manualBatteryProfileIsUsable(storedManualProfile)) {
      batteryGauge.setManualProfile(storedManualProfile);
    }
  }
  batteryGauge.service(millis());
  logBoot(String("boot_stage=battery_gauge_ready ") + batteryGauge.statusJson());
  imu.begin(deviceConfig.data().imuEnabled);
  logBoot(String("boot_stage=imu_ready ") + imu.statusJson());
  magnetometer.begin(imu.initialized());
  logBoot(String("boot_stage=magnetometer_ready ") + magnetometer.statusJson());

  if (!nhos::validatePinMap()) {
    logBoot("pin_map_invalid");
    criticalError = true;
    leds.setSignal(nhos::LedSignal::Error);
    leds.service(millis());
  }

  scanner.begin();
  if (!scanner.setLayout(
      deviceConfig.data().matrixLayout.analogPins,
      deviceConfig.data().matrixLayout.analogCount,
      deviceConfig.data().matrixLayout.selectPins,
      deviceConfig.data().matrixLayout.selectCount)) {
    logBoot("matrix_layout_invalid config_ignored=true");
  }
  scanner.setTiming(
      deviceConfig.data().scanTiming.targetFps,
      deviceConfig.data().scanTiming.settleUs,
      deviceConfig.data().scanTiming.sendEveryNFrames);
  imu.setServiceIntervalUs(scanner.scanIntervalUs());
  scanner.setStreamBufferConfig(
      deviceConfig.data().streamBuffer.enabled,
      deviceConfig.data().streamBuffer.depthFrames);
  scanner.setFilterConfig(deviceConfig.data().filter);
  scanner.setStreamRawAdc(deviceConfig.data().streamRawAdc);
  calibration.setLayout(
      deviceConfig.data().matrixLayout.analogPins,
      deviceConfig.data().matrixLayout.analogCount,
      deviceConfig.data().matrixLayout.selectPins,
      deviceConfig.data().matrixLayout.selectCount);
  calibration.begin(storage);
  scanner.setCalibration(&calibration);
  logBoot(String("boot_stage=scanner_ready shape=") + scanner.matrixShapeJson());
  if (bootMode.mode() == nhos::RunMode::Normal && scanner.hasLayout()) {
    scanner.start();
    logBoot("scan_task_started");
  } else if (!scanner.hasLayout()) {
    logBoot("scan_task_deferred matrix_layout_empty");
  } else {
    logBoot("scan_task_deferred maintenance_mode=true");
  }

  // esp_read_mac() reads straight from eFuse, unlike WiFi.macAddress()
  // (observed unreliable pre-esp_wifi_start() during New Horizons Direct
  // spike testing) -- safe to call before any WiFi/ESP-NOW init below.
  uint8_t uid[6] = {0};
  wifi.macBytes(uid);
  packetBuilder.setDeviceUid(uid);

  bool wifiConnected = false;
  espNowMode = deviceConfig.data().transport.mode == "espnow";
  // If a previous boot's ESP-NOW pairing attempt (EspNowPairing --
  // whether it was broadcast-discovering with an empty hub_mac, or
  // retrying a known-but-unreachable hub_mac) timed out without ever
  // pairing, don't retry a doomed attempt forever -- fall back to the
  // WiFi SoftAP portal so the device stays reachable for reconfiguration.
  // Deliberately checked once here at boot, not as a live mid-loop
  // transition -- see EspNowPairing.cpp's failPairingAndRestart() comment
  // for why (mirrors set_transport's own "next boot, not in-place" rule).
  // Checked on the flag alone, NOT also gated on hubMac.isEmpty() --
  // that used to only recognize the discovery case; now that a known
  // (but unreachable) hub_mac can also set this same flag, keeping that
  // condition would make this check never trip for that case, and the
  // device would restart every ~2 minutes forever instead of ever
  // reaching the portal.
  const bool espnowPairingPreviouslyFailed =
      espNowMode && storage.getUInt(nhos::kEspNowPairingFailFlagKey, 0) != 0;
  if (espnowPairingPreviouslyFailed) {
    espNowMode = false;
    logBoot("espnow_pairing_previous_timeout falling_back_to_wifi_portal");
  } else if (espNowMode && bootMode.wifiSetupRequested()) {
    // Holding the boot action button must win over the device's
    // persisted ESP-NOW/Hub transport mode, or there is no way back to
    // the WiFi setup portal (and so no way to switch back to
    // Gateway/WiFi mode) once a device has ever been paired with a Hub --
    // bootMode.wifiSetupRequested() used to only be consulted inside the
    // WiFi-mode branch below, which this device never reaches while
    // transport.mode == "espnow".
    espNowMode = false;
    logBoot("boot_action_button_setup_requested_from_espnow falling_back_to_wifi_portal");
  }
  if (espNowMode) {
    // ESP-NOW doesn't join a WiFi AP at all -- just needs the radio
    // driver initialized (see EspNowPairing.h for why the channel itself
    // isn't fixed here). FindMe/UDP streaming/heartbeat are meaningless
    // in this mode and are skipped entirely, not adapted.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    logBoot("boot_stage=wifi_skipped transport=espnow");
    if (!espNowPairing.begin(storage, deviceConfig, control, uid)) {
      logBoot("espnow_pairing_begin_failed");
    }
    espNowPairing.setArbiter(&airtime);
    espNowPairing.setOtaReceiver(&espNowOtaReceiver);
    espNowOtaReceiver.setArbiter(&airtime);
    espNowTransport.setArbiter(&airtime);
    airtime.registerClaimant(nhos::AirtimeClass::OtaRelay, &otaRelayNeedsAirtime, nullptr);
    airtime.registerClaimant(nhos::AirtimeClass::CommandResponse, &commandResponseNeedsAirtime, nullptr);
    esp_now_register_recv_cb(onEspNowRecv);
    esp_now_register_send_cb(onEspNowSent);
    espNowTransport.attach(espNowPairing);
    activeTransport = &espNowTransport;
    logBoot("boot_stage=espnow_pairing_ready");
  } else {
    wifiConnected = wifi.begin(storage, deviceConfig,
                                espnowPairingPreviouslyFailed || bootMode.wifiSetupRequested());
    if (wifiConnected) {
      bootMode.markWifiConnected();
    }
    logBoot(String("boot_stage=wifi_ready connected=") + (wifiConnected ? "true" : "false") +
            " setup_active=" + (wifi.setupActive() ? "true" : "false"));
    if (wifiConnected) {
      timeSync.begin();
    }
    findme.begin(storage, wifi, uid);
    logBoot("findme_started");
    // One bind only — see comment on udpTransport above.
    udpTransport.begin();
    udpTransport.attach(control, findme);
    activeTransport = &udpTransport;
    logBoot(String("udp_stream_started port=") + String(nhos::kUdpStreamPort));
  }
  ota.begin(storage);
  logBoot("boot_stage=ota_ready");
  serviceAutoOta(wifiConnected);
  control.begin(wifi, scanner, storage, bootMode, ota, findme, power, batteryGauge,
                powerState, imu, magnetometer, leds, deviceConfig, calibration,
                displayManager, externalLeds);
  control.setFaultRecorder(&faults);
  control.setScheduler(&scheduler);
  procFs.attach(scheduler, faults, scanner, bootMode, powerState, wifi, findme, deviceConfig);
  control.setProcFs(&procFs);
  control.setArbiter(&airtime);
  procFs.setArbiter(&airtime);
  control.setServiceManager(&services);
  procFs.setServiceManager(&services);
  procFs.setStorage(&storage);
  control.setClock(&timeSync);
  control.setPowerGovernor(&powerGovernor);
  procFs.setPowerGovernor(&powerGovernor);
  control.setAppManager(&apps, &flowApp, &appRegistry, &appGovernor);
  procFs.setAppManager(&apps);
  procFs.setAppRegistry(&appRegistry);
  procFs.setClock(&timeSync);
  if (espNowMode) {
    // Route check_update/apply_update through the Hub-relayed OTA path --
    // a Direct-mode device has no WiFi, so OtaManager's HTTP fetch can
    // only fail. See ControlServer::setEspNowOtaReceiver().
    control.setEspNowOtaReceiver(&espNowOtaReceiver);
  }
  logBoot(String("boot_stage=control_ready port=") + String(nhos::kControlPort));

  logBoot(String("runtime_ready protocol=") + nhos::kProtocolName + " firmware=" + nhos::kFirmwareVersion +
          " mode=" + bootMode.modeName());
  // Only now: setup() deliberately blocks for seconds at a time
  // (sampleWifiSetupButtonWindow's 3s window, serviceAutoOta's HTTP
  // download), and the 5s task WDT would fire straight through those.
  // Until this call the loopTask is watched by nothing at all -- it runs on
  // CPU1 and the Arduino core only subscribes CPU0's idle task, so any hang
  // inside loop() used to wedge the device silently and forever.
  nhos::watchdogArm();
  logBoot(String("boot_stage=loop_wdt_armed timeout_s=") + String(CONFIG_ESP_TASK_WDT_TIMEOUT_S));
  bootMode.markBootOk();
  logBoot("boot_ok_marked");
  // SafeMaintenance means earlier boots already failed repeatedly, so this
  // one is not evidence the image is good -- leave it on probation and let
  // the bootloader revert on the next reset. criticalError likewise (an
  // invalid pin map means the device never really came up).
  if (bootMode.otaPendingVerify()) {
    if (bootMode.mode() != nhos::RunMode::SafeMaintenance && !criticalError) {
      logBoot(bootMode.confirmFirmwareValid() ? "ota_image_confirmed"
                                              : "ota_image_confirm_failed");
    } else {
      logBoot(String("ota_image_left_pending mode=") + bootMode.modeName() +
              " critical_error=" + (criticalError ? "true" : "false"));
    }
  }
  apps.begin(&storage);
  apps.setLedSink(&applyAppLed);
  appGovernor.begin(&apps);
  {
    const uint16_t cellCount =
        scanner.health().pointCount != 0 ? scanner.health().pointCount : nhos::kMaxSensors;
    static const char* kSlotNames[nhos::AppRegistry::kMaxSlots] = {"flow", "flow1", "flow2",
                                                                   "flow3"};
    nhos::FlowApp* slotPtrs[nhos::AppRegistry::kMaxSlots];
    for (uint8_t i = 0; i < nhos::AppRegistry::kMaxSlots; ++i) {
      flowApps[i].attach(storage);
      // MUST precede install(): install() indexes by name and persists
      // app_en_<name>, so a slot renamed afterwards would be unreachable.
      flowApps[i].setIdentity(kSlotNames[i]);
      slotPtrs[i] = &flowApps[i];
    }
    for (uint8_t i = 0; i < nhos::AppRegistry::kMaxSlots; ++i) {
      apps.install(slotPtrs[i]);
    }
    appRegistry.begin(storage, apps, slotPtrs, cellCount);
    appRegistry.restore();
    logBoot(String("boot_stage=packages_ready ") + appRegistry.statusJson());

    // A standalone graph is optional: a device with no packages and no
    // /files/apps/flow.json simply runs with empty slots.
    if (!flowApp.loaded()) {
      String flowError;
      if (flowApp.loadFromFile("apps/flow.json", cellCount, flowError)) {
        logBoot(String("flow_graph_loaded ") + flowApp.statusJson(false));
      } else if (flowError != "file_not_found") {
        logBoot(String("flow_graph_rejected reason=") + flowError);
      }
    }
  }
  logBoot(String("boot_stage=apps_ready ") + apps.statusJson());
  registerServices();
  logBoot(String("boot_stage=services_ready ") + services.statusJson());
  registerRuntimeTasks();
  logBoot(String("boot_stage=scheduler_ready ") + scheduler.statusJson());
  // What setup() left for everything that allocates later -- WiFi's large
  // buffers in particular -- on a board with no PSRAM. The same figures as
  // memory_status, but readable over USB without a network.
  logBoot(String("boot_heap free=") + ESP.getFreeHeap() + " largest=" + ESP.getMaxAllocHeap() +
          " min=" + ESP.getMinFreeHeap());
  updateLedState();
}

void loop() {
  scheduler.tick();

  if (bootMode.rebootRequested()) {
    delay(100);
    ESP.restart();
  }
  // Unchanged idle heuristic: hand the CPU to the WiFi stack and background
  // tasks whenever the next scan is not imminent.
  if (!scanner.active() || !scanner.hasLayout() ||
      static_cast<int32_t>(scanner.nextScanDueUs() - micros()) > 2000) {
    yield();
  }
}
