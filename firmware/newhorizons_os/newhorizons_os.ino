#include <Arduino.h>
#include <WiFiUdp.h>
#include <Wire.h>

#include "BoardPins.h"
#include "BootModeManager.h"
#include "Config.h"
#include "ControlServer.h"
#include "DeviceConfig.h"
#include "DisplayManager.h"
#include "ExternalLedController.h"
#include "FindMeClient.h"
#include "ImuManager.h"
#include "LedController.h"
#include "MatrixScanner.h"
#include "OtaManager.h"
#include "PacketBuilder.h"
#include "PowerManager.h"
#include "Storage.h"
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
nhos::PacketBuilder packetBuilder;
nhos::PowerManager power;
nhos::ImuManager imu;
nhos::FindMeClient findme;
nhos::ControlServer control;
nhos::OtaManager ota;
WiFiUDP streamUdp;

uint8_t packetBuffer[
    nhos::kPacketHeaderLen +
    (nhos::kMaxSensors * sizeof(float)) +
    (7 * sizeof(float)) +
    4 +
    nhos::kPacketHmacLen
];

uint32_t heartbeatSeq = 1;
uint32_t lastHeartbeatAttemptMs = 0;
bool criticalError = false;
uint32_t lastObservedOverrunFrames = 0;
uint32_t lastObservedUdpFailures = 0;
uint32_t scanWarningUntilMs = 0;

void logBoot(const String& message) {
  Serial.println(message);
  storage.logLine(String(millis()) + " " + message);
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

void scanAndStreamIfDue() {
  if (!wifi.isConnected() || control.maintenanceMode()) {
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

  float imuSample[7] = {0};
  const bool imuSampleValid = imu.readSample(imuSample);
  size_t len = packetBuilder.buildMatrixPacketHeader(frame, packetBuffer, sizeof(packetBuffer), matrixPayloadLen, imuSampleValid ? imuSample : nullptr);
  if (!len) {
    scanner.recordUdpSend(false, 0);
    return;
  }
  String host = control.streamHost();
  uint16_t port = control.streamPort();
  if (findme.hasGateway()) {
    host = findme.streamHost();
    port = findme.streamPort();
  }
  if (host.isEmpty()) {
    return;
  }
  const uint32_t udpStartUs = micros();
  streamUdp.beginPacket(host.c_str(), port);
  streamUdp.write(packetBuffer, len);
  const bool sent = streamUdp.endPacket() == 1;
  scanner.recordUdpSend(sent, micros() - udpStartUs);
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
  size_t len = packetBuilder.buildHeartbeat(heartbeatSeq++, now, packetBuffer, sizeof(packetBuffer));
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
  nhos::LedSignal activeSignal = nhos::LedSignal::Online;
  if (health.overrunFrames > lastObservedOverrunFrames || health.udpSendFailures > lastObservedUdpFailures) {
    scanWarningUntilMs = nowMs + 10000;
    lastObservedOverrunFrames = health.overrunFrames;
    lastObservedUdpFailures = health.udpSendFailures;
  }

  if (criticalError) {
    activeSignal = nhos::LedSignal::Error;
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
  } else {
    if (scanWarningUntilMs && static_cast<int32_t>(nowMs - scanWarningUntilMs) < 0) {
      activeSignal = nhos::LedSignal::ScanWarning;
    } else if (ESP.getFreeHeap() < 30000 || ESP.getMaxAllocHeap() < 12000) {
      activeSignal = nhos::LedSignal::RamDanger;
    } else if (power.chargeState() == nhos::ChargeState::ChargingOrMissing) {
      activeSignal = nhos::LedSignal::ChargingOrMissing;
    } else if (power.chargeState() == nhos::ChargeState::ChargeDone) {
      activeSignal = nhos::LedSignal::ChargeDone;
    } else {
      activeSignal = nhos::LedSignal::Online;
    }
  }
  leds.setSignal(activeSignal);
  leds.service(nowMs);
  externalLeds.service(nowMs, health, activeSignal);
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
  Wire.begin(nhos::kI2cSda, nhos::kI2cScl, 400000);
  logBoot(String("boot_stage=i2c_ready sda=") + String(nhos::kI2cSda) + " scl=" + String(nhos::kI2cScl));
  displayManager.begin(deviceConfig.data().oled);
  logBoot(String("boot_stage=display_ready ") + displayManager.statusJson());
  power.begin(storage.getString("charge_profile", "compatible"));
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
  logBoot(String("boot_stage=scanner_ready shape=") + scanner.matrixShapeJson());
  if (bootMode.mode() == nhos::RunMode::Normal && scanner.hasLayout()) {
    scanner.start();
    logBoot("scan_task_started");
  } else if (!scanner.hasLayout()) {
    logBoot("scan_task_deferred matrix_layout_empty");
  } else {
    logBoot("scan_task_deferred maintenance_mode=true");
  }

  bool wifiConnected = wifi.begin(storage, bootMode.wifiSetupRequested());
  logBoot(String("boot_stage=wifi_ready connected=") + (wifiConnected ? "true" : "false") +
          " setup_active=" + (wifi.setupActive() ? "true" : "false"));
  uint8_t uid[6] = {0};
  wifi.macBytes(uid);
  packetBuilder.setDeviceUid(uid);
  findme.begin(storage, wifi, uid);
  logBoot("findme_started");
  streamUdp.begin(nhos::kUdpStreamPort);
  logBoot(String("udp_stream_started port=") + String(nhos::kUdpStreamPort));
  ota.begin(storage);
  logBoot("boot_stage=ota_ready");
  serviceAutoOta(wifiConnected);
  control.begin(wifi, scanner, storage, bootMode, ota, findme, power, imu, leds, deviceConfig, displayManager, externalLeds);
  logBoot(String("boot_stage=control_ready port=") + String(nhos::kControlPort));

  logBoot(String("runtime_ready protocol=") + nhos::kProtocolName + " firmware=" + nhos::kFirmwareVersion +
          " mode=" + bootMode.modeName());
  bootMode.markBootOk();
  logBoot("boot_ok_marked");
  updateLedState();
}

void loop() {
  wifi.service();
  power.service(millis());
  findme.setModeName(bootMode.modeName());
  findme.service();
  control.service();
  sendHeartbeatIfDue();
  scanAndStreamIfDue();
  updateLedState();
  displayManager.service(
      millis(),
      wifi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString(),
      findme.hasGateway() ? findme.streamHost() : String("-"),
      scanner.health(),
      ESP.getFreeHeap(),
      ESP.getHeapSize());
  if (bootMode.rebootRequested()) {
    delay(100);
    ESP.restart();
  }
  delay(1);
}
