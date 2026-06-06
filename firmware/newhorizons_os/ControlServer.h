#pragma once

#include <Arduino.h>
#include <WiFiServer.h>
#include <vector>

#include "BootModeManager.h"
#include "Calibration.h"
#include "DeviceConfig.h"
#include "DisplayManager.h"
#include "ExternalLedController.h"
#include "FindMeClient.h"
#include "ImuManager.h"
#include "LedController.h"
#include "MatrixScanner.h"
#include "OtaManager.h"
#include "PowerManager.h"
#include "PowerStateManager.h"
#include "Storage.h"
#include "WifiManager.h"

namespace nhos {

class PowerStateManager;

class ControlServer {
 public:
  void begin(
      WifiManager& wifi,
      MatrixScanner& scanner,
      Storage& storage,
      BootModeManager& boot,
      OtaManager& ota,
      FindMeClient& findme,
      PowerManager& power,
      PowerStateManager& powerState,
      ImuManager& imu,
      LedController& leds,
      DeviceConfig& deviceConfig,
      Calibration& calibration,
      DisplayManager& display,
      ExternalLedController& externalLeds);
  void service();
  bool maintenanceMode() const;
  const String& streamHost() const;
  uint16_t streamPort() const;

 private:
  void servicePendingApplyUpdate();
  String processCommand(const String& request);
  String ok(const String& command, const String& message, const String& data = "{}") const;
  String error(const String& command, const String& message) const;
  String deviceUidString() const;
  String commandName(const String& request) const;
  String scanTimingStatusJson() const;
  String layoutStatusJson() const;
  String indicatorsStatusJson() const;
  String extractString(const String& request, const char* key) const;
  int extractInt(const String& request, const char* key, int fallback) const;
  float extractFloat(const String& request, const char* key, float fallback) const;
  bool extractBool(const String& request, const char* key, bool fallback) const;
  size_t extractArray(const String& request, const char* key, uint8_t* out, size_t maxCount) const;
  String extractObject(const String& request, const char* key) const;
  bool requireMaintenance(const String& command) const;
  String fileSizeJson(const String& command, const String& scope, const String& path) const;
  String fileChunkJson(const String& command, const String& scope, const String& path, size_t offset, size_t length) const;
  bool decodeHex(const String& hex, std::vector<uint8_t>& out) const;
  String encodeHex(const std::vector<uint8_t>& data) const;

  WiFiServer server_{kControlPort};
  WifiManager* wifi_ = nullptr;
  MatrixScanner* scanner_ = nullptr;
  Storage* storage_ = nullptr;
  BootModeManager* boot_ = nullptr;
  OtaManager* ota_ = nullptr;
  FindMeClient* findme_ = nullptr;
  PowerManager* power_ = nullptr;
  PowerStateManager* powerState_ = nullptr;
  ImuManager* imu_ = nullptr;
  LedController* leds_ = nullptr;
  DeviceConfig* deviceConfig_ = nullptr;
  Calibration* calibration_ = nullptr;
  DisplayManager* display_ = nullptr;
  ExternalLedController* externalLeds_ = nullptr;
  bool started_ = false;
  String streamHost_;
  uint16_t streamPort_ = kUdpStreamPort;
  String writeScope_;
  String writePath_;
  size_t writeExpectedSize_ = 0;
  size_t writeWritten_ = 0;
  bool otaPending_ = false;
  String otaPendingManifestUrl_;
};

}  // namespace nhos
