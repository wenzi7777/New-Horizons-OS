#include <Arduino.h>
#include <WiFiUdp.h>
#include <Wire.h>

#include "BoardPins.h"
#include "BootModeManager.h"
#include "Config.h"
#include "ControlServer.h"
#include "FindMeClient.h"
#include "LedController.h"
#include "MatrixScanner.h"
#include "OtaManager.h"
#include "PacketBuilder.h"
#include "Storage.h"
#include "WifiManager.h"

namespace {

nhos::Storage storage;
nhos::BootModeManager bootMode;
nhos::LedController leds;
nhos::WifiManager wifi;
nhos::MatrixScanner scanner;
nhos::PacketBuilder packetBuilder;
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

void logBoot(const String& message) {
  Serial.println(message);
  storage.logLine(String(millis()) + " " + message);
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

  size_t len = packetBuilder.buildMatrixPacketHeader(frame, packetBuffer, sizeof(packetBuffer), matrixPayloadLen);
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
  if (bootMode.mode() == nhos::RunMode::SafeMaintenance) {
    leds.setStatus(nhos::LedPalette::SafeMode);
  } else if (control.maintenanceMode()) {
    leds.setStatus(nhos::LedPalette::Maintenance);
  } else if (wifi.setupActive()) {
    leds.setStatus(nhos::LedPalette::WifiSetup);
  } else if (wifi.isConnected()) {
    leds.setStatus(nhos::LedPalette::Online);
  } else {
    leds.setStatus(nhos::LedPalette::Boot);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("New Horizons OS Arduino boot");

  storage.begin();
  logBoot("boot_stage=storage_ready");
  leds.begin();
  logBoot("boot_stage=leds_ready");
  bootMode.begin();
  logBoot(String("boot_stage=boot_mode_ready mode=") + bootMode.modeName());
  Wire.begin(nhos::kI2cSda, nhos::kI2cScl, 400000);
  logBoot(String("boot_stage=i2c_ready sda=") + String(nhos::kI2cSda) + " scl=" + String(nhos::kI2cScl));

  if (!nhos::validatePinMap()) {
    logBoot("pin_map_invalid");
    leds.setStatus(nhos::LedPalette::Error);
  }

  scanner.begin();
  logBoot(String("boot_stage=scanner_ready rows=") + String(nhos::kRows) + " cols=" + String(nhos::kCols));
  if (bootMode.mode() == nhos::RunMode::Normal) {
    scanner.start();
    logBoot("scan_task_started");
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
  control.begin(wifi, scanner, storage, bootMode, ota, findme);
  logBoot(String("boot_stage=control_ready port=") + String(nhos::kControlPort));

  logBoot(String("runtime_ready protocol=") + nhos::kProtocolName + " firmware=" + nhos::kFirmwareVersion +
          " mode=" + bootMode.modeName());
  bootMode.markBootOk();
  logBoot("boot_ok_marked");
  updateLedState();
}

void loop() {
  wifi.service();
  findme.setModeName(bootMode.modeName());
  findme.service();
  control.service();
  sendHeartbeatIfDue();
  scanAndStreamIfDue();
  updateLedState();
  if (bootMode.rebootRequested()) {
    delay(100);
    ESP.restart();
  }
  delay(1);
}
