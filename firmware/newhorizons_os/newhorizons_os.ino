#include <Arduino.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <esp_now.h>

#include "BoardPins.h"
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
#include "FindMeClient.h"
#include "ImuManager.h"
#include "LedController.h"
#include "MatrixScanner.h"
#include "OtaManager.h"
#include "PacketBuilder.h"
#include "PowerAnimation.h"
#include "PowerManager.h"
#include "PowerStateManager.h"
#include "StreamTransport.h"
#include "Storage.h"
#include "TimeSync.h"
#include "UdpStreamTransport.h"
#include "WifiManager.h"

namespace {

nhos::Storage storage;
nhos::DeviceConfig deviceConfig;
nhos::BootModeManager bootMode;
nhos::LedController leds;
nhos::ExternalLedController externalLeds;
nhos::DisplayManager displayManager;
nhos::WifiManager wifi;
nhos::MatrixScanner scanner;
nhos::Calibration calibration;
nhos::PacketBuilder packetBuilder;
nhos::PowerManager power;
nhos::PowerStateManager powerState;
nhos::ImuManager imu;
nhos::FindMeClient findme;
nhos::ControlServer control;
nhos::OtaManager ota;
nhos::TimeSync timeSync;
WiFiUDP streamUdp;
nhos::UdpStreamTransport udpTransport;
nhos::EspNowStreamTransport espNowTransport;
nhos::EspNowPairing espNowPairing;
nhos::EspNowOtaReceiver espNowOtaReceiver;
nhos::StreamTransport* activeTransport = nullptr;
bool espNowMode = false;

void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  espNowPairing.handleEspNowRecv(info->src_addr, data, static_cast<size_t>(len));
}

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

// Bug-3 diagnostics: per-section loop timing, to find what caps the
// achievable IMU poll rate below whatever serviceIntervalUs_ requests.
// Reported periodically over serial; remove once the bottleneck is found
// and fixed for good.
struct LoopProfile {
  uint32_t totalUs = 0;
  uint32_t totalMaxUs = 0;
  uint32_t scanUs = 0;
  uint32_t wifiUs = 0;
  uint32_t findmeUs = 0;
  uint32_t controlUs = 0;
  uint32_t imuUs = 0;
  uint32_t ledUs = 0;
  uint32_t displayUs = 0;
  uint32_t iterations = 0;
};
LoopProfile loopProfile;
uint32_t lastProfileReportMs = 0;
constexpr uint32_t kProfileReportIntervalMs = 3000;

void reportLoopProfileIfDue(uint32_t nowMs) {
  if (nowMs - lastProfileReportMs < kProfileReportIntervalMs) {
    return;
  }
  lastProfileReportMs = nowMs;
  if (loopProfile.iterations == 0) {
    return;
  }
  const uint32_t n = loopProfile.iterations;
  Serial.print(F("loop_profile n="));
  Serial.print(n);
  Serial.print(F(" avg_total_us="));
  Serial.print(loopProfile.totalUs / n);
  Serial.print(F(" max_total_us="));
  Serial.print(loopProfile.totalMaxUs);
  Serial.print(F(" avg_scan_us="));
  Serial.print(loopProfile.scanUs / n);
  Serial.print(F(" avg_wifi_us="));
  Serial.print(loopProfile.wifiUs / n);
  Serial.print(F(" avg_findme_us="));
  Serial.print(loopProfile.findmeUs / n);
  Serial.print(F(" avg_control_us="));
  Serial.print(loopProfile.controlUs / n);
  Serial.print(F(" avg_imu_us="));
  Serial.print(loopProfile.imuUs / n);
  Serial.print(F(" avg_led_us="));
  Serial.print(loopProfile.ledUs / n);
  Serial.print(F(" avg_display_us="));
  Serial.print(loopProfile.displayUs / n);
  Serial.print(F(" imu_measured_hz="));
  Serial.println(imu.measuredSampleRateHz());
  loopProfile = LoopProfile();
}

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
  logBoot("auto_ota_enabled");
  leds.setSignal(nhos::LedSignal::OtaActive);
  leds.service(millis());
  const bool applied = ota.autoApplyIfNewer(deviceConfig.data().ota.manifestUrl);
  if (!applied) {
    if (ota.lastPhase() == "current") {
      return;
    }
    logBoot(String("auto_ota_apply_failed status=") + ota.lastStatusJson());
    leds.showEvent(nhos::LedSignal::OtaError);
    leds.service(millis());
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
    setCpuFrequencyMhz(80);
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
  setCpuFrequencyMhz(240);
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
  powerState.service(millis(), power.chargerDetected(), power.chargeState());
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
  return espNowMode || wifi.isConnected();
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
  const bool epochValid = timeSync.hasSynced();
  const uint64_t epochMs = epochValid ? timeSync.nowEpochMs() : 0;
  size_t len = packetBuilder.buildMatrixPacketHeader(frame, epochMs, epochValid, packetBuffer, sizeof(packetBuffer), matrixPayloadLen, imuSampleValid ? imuSample : nullptr);
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
  streamUdp.beginPacket(findme.streamHost().c_str(), findme.streamPort());
  streamUdp.write(packetBuffer, len);
  if (streamUdp.endPacket()) {
    findme.recordHeartbeat(now, "");
  } else {
    findme.recordHeartbeat(now, "heartbeat_send_failed");
  }
}

void updateLedState() {
  const uint32_t nowMs = millis();
  const nhos::ScanHealth health = scanner.health();
  nhos::ExternalLedInputs extIn;
  extIn.wifiConnected = wifi.isConnected();
  extIn.wifiBusy = wifi.setupActive();
  extIn.hasGateway = findme.hasGateway();
  extIn.pressure01 = scanner.lastPeak01();
  extIn.calibrating = calibration.sessionActive();
  if (health.overrunFrames > lastObservedOverrunFrames || health.udpSendFailures > lastObservedUdpFailures) {
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
    if (power.chargeState() == nhos::ChargeState::ChargingOrMissing) {
      leds.setSignal(nhos::LedSignal::SoftOffCharging);
    } else if (power.chargeState() == nhos::ChargeState::ChargeDone) {
      leds.setSignal(nhos::LedSignal::SoftOffChargeDone);
    } else {
      leds.setSignal(nhos::LedSignal::Off);
    }
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
  } else if (bootMode.mode() == nhos::RunMode::SafeMaintenance) {
    activeSignal = nhos::LedSignal::SafeMode;
  } else if (control.maintenanceMode()) {
    activeSignal = nhos::LedSignal::Maintenance;
  } else if (wifi.setupActive()) {
    activeSignal = nhos::LedSignal::WifiSetup;
  } else if (!wifi.isConnected()) {
    activeSignal = nhos::LedSignal::WifiConnecting;
  } else if (!findme.hasGateway()) {
    activeSignal = nhos::LedSignal::FindMePending;
  } else if (power.chargeState() == nhos::ChargeState::ChargeDone) {
    activeSignal = nhos::LedSignal::ChargeDone;
  } else if (power.chargeState() == nhos::ChargeState::ChargingOrMissing) {
    activeSignal = nhos::LedSignal::ChargingOrMissing;
  } else {
    activeSignal = nhos::LedSignal::Online;
  }
  leds.setSignal(activeSignal);
  leds.service(nowMs);
  extIn.systemSignal = activeSignal;
  externalLeds.service(nowMs, health, extIn);
}

}  // namespace

void setup() {
  Serial.begin(115200);
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
  externalLeds.begin(deviceConfig.data().externalLed);
  logBoot("boot_stage=leds_ready");
  bootMode.begin();
  logBoot(String("boot_stage=boot_mode_ready mode=") + bootMode.modeName());
  powerState.begin();
  logBoot(String("boot_stage=power_state_ready ") + powerState.statusJson());
  Wire.begin(nhos::kI2cSda, nhos::kI2cScl, NHOS_BOARD_I2C_HZ);
  logBoot(String("boot_stage=i2c_ready sda=") + String(nhos::kI2cSda) + " scl=" + String(nhos::kI2cScl));
  displayManager.begin(deviceConfig.data().oled);
  logBoot(String("boot_stage=display_ready ") + displayManager.statusJson());
  power.begin(storage.getString("charge_profile", "slow"));
  logBoot(String("boot_stage=power_ready ") + power.statusJson());
  imu.begin(deviceConfig.data().imuEnabled);
  logBoot(String("boot_stage=imu_ready ") + imu.statusJson());

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
  // If a previous boot's broadcast-discovery bootstrap (EspNowPairing,
  // triggered when hub_mac was empty) timed out without finding any Hub,
  // don't retry a doomed discovery forever -- fall back to the WiFi
  // SoftAP portal so the device stays reachable for reconfiguration.
  // Deliberately checked once here at boot, not as a live mid-loop
  // transition -- see EspNowPairing.cpp's serviceDiscovery() comment for
  // why (mirrors set_transport's own "next boot, not in-place" rule).
  const bool espnowDiscoveryPreviouslyFailed =
      espNowMode && deviceConfig.data().transport.hubMac.isEmpty() &&
      storage.getUInt(nhos::kEspNowDiscoveryFailFlagKey, 0) != 0;
  if (espnowDiscoveryPreviouslyFailed) {
    espNowMode = false;
    logBoot("espnow_discovery_previous_timeout falling_back_to_wifi_portal");
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
    espNowPairing.setOtaReceiver(&espNowOtaReceiver);
    esp_now_register_recv_cb(onEspNowRecv);
    espNowTransport.attach(espNowPairing);
    activeTransport = &espNowTransport;
    logBoot("boot_stage=espnow_pairing_ready");
  } else {
    wifiConnected = wifi.begin(storage, deviceConfig,
                                espnowDiscoveryPreviouslyFailed || bootMode.wifiSetupRequested());
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
    streamUdp.begin(nhos::kUdpStreamPort);
    udpTransport.begin();
    udpTransport.attach(control, findme);
    activeTransport = &udpTransport;
    logBoot(String("udp_stream_started port=") + String(nhos::kUdpStreamPort));
  }
  ota.begin(storage);
  logBoot("boot_stage=ota_ready");
  serviceAutoOta(wifiConnected);
  control.begin(wifi, scanner, storage, bootMode, ota, findme, power, powerState, imu, leds, deviceConfig, calibration, displayManager, externalLeds);
  logBoot(String("boot_stage=control_ready port=") + String(nhos::kControlPort));

  logBoot(String("runtime_ready protocol=") + nhos::kProtocolName + " firmware=" + nhos::kFirmwareVersion +
          " mode=" + bootMode.modeName());
  bootMode.markBootOk();
  logBoot("boot_ok_marked");
  updateLedState();
}

void loop() {
  const uint32_t loopStartUs = micros();
  power.service(millis());
  servicePowerState();
  if (powerState.shouldRunServices()) {
    uint32_t sectionStartUs = micros();
    scanAndStreamIfDue();
    sendQueuedPacketIfAny();
    loopProfile.scanUs += micros() - sectionStartUs;

    sectionStartUs = micros();
    if (espNowMode) {
      espNowPairing.service();
      espNowOtaReceiver.service();
      espNowTransport.service();
    } else {
      wifi.service();
    }
    loopProfile.wifiUs += micros() - sectionStartUs;

    if (!espNowMode) {
      findme.setModeName(bootMode.modeName());
      sectionStartUs = micros();
      findme.service();
      loopProfile.findmeUs += micros() - sectionStartUs;
    }

    sectionStartUs = micros();
    control.service();
    if (!espNowMode) {
      control.serviceUdpCommand(streamUdp);
    }
    loopProfile.controlUs += micros() - sectionStartUs;

    sectionStartUs = micros();
    imu.service(micros());
    loopProfile.imuUs += micros() - sectionStartUs;

    if (!espNowMode) {
      if (wifi.isConnected()) {
        timeSync.begin();  // no-op after first successful call; covers WiFi connecting after boot
      }
      sendHeartbeatIfDue();
    }

    sectionStartUs = micros();
    updateLedState();
    loopProfile.ledUs += micros() - sectionStartUs;

    servicePowerTransition();

    sectionStartUs = micros();
    displayManager.service(
        millis(),
        wifi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString(),
        findme.hasGateway() ? findme.streamHost() : String("-"),
        scanner.health(),
        ESP.getFreeHeap(),
        ESP.getHeapSize());
    loopProfile.displayUs += micros() - sectionStartUs;
  } else {
    if (powerState.transitionPhase() != nhos::PowerTransitionPhase::None) {
      servicePowerTransition();
    } else {
      applySoftOffIndicators();
      updateLedState();
      // powerState.lightSleep() emits:
      // soft_off_sleep_enter state=
      // soft_off_wake cause=
      powerState.lightSleep();
    }
  }
  if (bootMode.rebootRequested()) {
    delay(100);
    ESP.restart();
  }
  const uint32_t thisIterationUs = micros() - loopStartUs;
  loopProfile.totalUs += thisIterationUs;
  if (thisIterationUs > loopProfile.totalMaxUs) {
    loopProfile.totalMaxUs = thisIterationUs;
  }
  ++loopProfile.iterations;
  reportLoopProfileIfDue(millis());
  if (!scanner.active() || !scanner.hasLayout() ||
      static_cast<int32_t>(scanner.nextScanDueUs() - micros()) > 2000) {
    yield();
  }
}
