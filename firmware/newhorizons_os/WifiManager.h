#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "Config.h"
#include "SetupPortal.h"
#include "Storage.h"

namespace nhos {

class DeviceConfig;

class WifiManager {
 public:
  bool begin(Storage& storage, DeviceConfig& deviceConfig, bool forceSetupPortal = false);
  void service();
  void suspend();
  void resume();
  bool isConnected() const;
  bool setupActive() const;
  bool hasCredentials() const;
  bool applyCredentials(const String& ssid, const String& password);
  void clearCredentials();
  String statusJson() const;
  void macBytes(uint8_t out[6]) const;

 private:
  bool connectStored();
  void startSetupAp();
  void stopSetupPortal();
  void serviceSetupPortal();
  SetupPortalHooks portalHooks();
  // Warn, not Error: a lost AP is routine for a wearable, but the default
  // "error" flash log keeps neither unless a tag override asks for warn.
  void logWarn(const char* line);

  Storage* storage_ = nullptr;
  DeviceConfig* deviceConfig_ = nullptr;
  bool setupActive_ = false;
  bool suspended_ = false;
  bool portalStarted_ = false;
  uint32_t lastReconnectMs_ = 0;
  // 0 means "currently connected" (or not yet noticed a disconnect) --
  // stamped the first service() tick isConnected() is false, reset back
  // to 0 once reconnected. See service()'s kWifiReconnectFallbackMs check.
  uint32_t disconnectedSinceMs_ = 0;
  SetupPortal portal_;
};

}  // namespace nhos
