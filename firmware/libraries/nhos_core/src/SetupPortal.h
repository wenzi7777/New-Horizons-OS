#pragma once

// The SoftAP setup portal the device firmware and the Hub both run: open AP,
// captive DNS, and one static page (setup_portal.html, gzipped into flash)
// that talks to a few small JSON endpoints.
//
//   GET  /  /portal  /hotspot-detect.html   the page
//   GET  /api/info                          identity + saved settings (never a password or token)
//   GET  /api/scan[?refresh=1]              cached network list; refresh starts an async scan
//   POST /api/save                          validate; then test the Wi-Fi with the AP still up
//   GET  /api/status                        idle / connecting / ok / failed (+ reason)
//   POST /api/reset                         factory reset, when the product offers one
//
// Nothing here blocks loop(): the scan is async and the connect test is
// polled from service(). The products keep everything product-specific
// (what to validate, what to store, the AP name) in the Hooks.

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>

#include <functional>
#include <vector>

#include "WifiScanList.h"

namespace nhos {

struct SetupPortalIdentity {
  const char* product = "device";  // "device" | "hub" -- picks the page's colors and sections
  const char* variant = "full";    // "full" | "wifi_only"
  const char* board = "";
  const char* version = "";
};

enum class SetupPortalSave : uint8_t {
  Invalid,   // errors were appended; nothing changes
  TestWifi,  // try ssid/password with the AP still up, then onConnected()
  Restart,   // the product already saved what it needed; reboot
};

struct SetupPortalHooks {
  // Append product fields to the /api/info object, each starting with ','.
  std::function<void(String& json)> info;
  // Read the form from the server's args. On Invalid, append "field":"code"
  // pairs (comma-separated, no braces) to errors. On TestWifi, fill
  // ssid/password with what to try.
  std::function<SetupPortalSave(WebServer& server, String& errors, String& ssid, String& password)> save;
  // The tested Wi-Fi works: persist it and anything save() held back. The
  // portal reboots a few seconds later, once the page has seen "ok".
  std::function<void(const String& ssid, const String& password)> onConnected;
  // Optional. Runs right after the Wi-Fi test succeeds; return a warning
  // code for the page (e.g. "backend_unreachable") or nullptr.
  std::function<const char*()> probe;
  // Optional. Present means the page shows a factory reset button.
  std::function<void()> factoryReset;
};

class SetupPortal {
 public:
  void begin(const char* apSsid, const char* domain, const SetupPortalIdentity& identity,
             SetupPortalHooks hooks);
  void stop();
  void service();
  bool active() const { return active_; }

 private:
  enum class TestState : uint8_t { Idle, Connecting, Ok, Failed };

  void configureRoutes();
  void sendJson(int code, const String& body);
  void handlePage();
  void handleRedirect();
  void handleInfo();
  void handleScan();
  void handleSave();
  void handleStatus();
  void handleReset();
  void startScan();
  void collectScan();
  void serviceTest();

  WebServer server_{80};
  DNSServer dns_;
  SetupPortalIdentity identity_;
  SetupPortalHooks hooks_;
  String apSsid_;
  String domain_;
  bool active_ = false;
  bool routesConfigured_ = false;

  std::vector<WifiScanEntry> networks_;
  bool scanning_ = false;
  uint32_t lastScanMs_ = 0;

  TestState testState_ = TestState::Idle;
  String testSsid_;
  String testPassword_;
  uint32_t testStartMs_ = 0;
  const char* failCode_ = "";
  const char* warning_ = "";
  uint32_t restartAtMs_ = 0;
};

}  // namespace nhos
