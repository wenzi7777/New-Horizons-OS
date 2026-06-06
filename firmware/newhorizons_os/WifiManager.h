#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "Config.h"
#include "Storage.h"

namespace nhos {

class WifiManager {
 public:
  bool begin(Storage& storage, bool forceSetupPortal = false);
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
  void configurePortalRoutes();
  void serviceSetupPortal();
  void handlePortalRoot();
  void handlePortalSave();
  void handlePortalRedirect();
  String portalPage(const String& message, bool success) const;
  String wifiNetworkOptionsHtml() const;
  String htmlEscape(const String& value) const;

  Storage* storage_ = nullptr;
  bool setupActive_ = false;
  bool suspended_ = false;
  bool portalStarted_ = false;
  bool portalRoutesConfigured_ = false;
  uint32_t lastReconnectMs_ = 0;
  DNSServer dnsServer_;
  WebServer portalServer_{kSetupPortalPort};
};

}  // namespace nhos
