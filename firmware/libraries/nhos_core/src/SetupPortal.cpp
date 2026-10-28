#include "SetupPortal.h"

#include <WiFi.h>
#include <esp_mac.h>

#include "JsonUtils.h"
#include "SetupPortalAsset.h"
#include "StaConnectReason.h"

namespace nhos {

namespace {
constexpr uint32_t kRescanMinIntervalMs = 10000;
constexpr uint32_t kScanMsPerChannel = 120;
constexpr size_t kMaxListedNetworks = 16;
// A wrong password or a missing network is reported within a couple of
// seconds; anything else gets the full window before it counts as failed.
constexpr uint32_t kTestEarlyFailMs = 3000;
constexpr uint32_t kTestTimeoutMs = 15000;
// Long enough for the page's 1 s status poll to see "ok" before the reboot.
constexpr uint32_t kRestartAfterOkMs = 4000;
constexpr uint32_t kRestartAfterSaveMs = 1500;

// Written from the Wi-Fi event task, read from loop().
volatile uint8_t gLastDisconnectReason = 0;
bool gDisconnectHandlerRegistered = false;

const char* testStateName(uint8_t state) {
  switch (state) {
    case 1: return "connecting";
    case 2: return "ok";
    case 3: return "failed";
    default: return "idle";
  }
}
}  // namespace

void SetupPortal::begin(const char* apSsid, const char* domain, const SetupPortalIdentity& identity,
                        SetupPortalHooks hooks) {
  identity_ = identity;
  hooks_ = std::move(hooks);
  apSsid_ = apSsid;
  domain_ = domain;
  testState_ = TestState::Idle;
  restartAtMs_ = 0;
  if (!gDisconnectHandlerRegistered) {
    WiFi.onEvent(
        [](arduino_event_id_t, arduino_event_info_t info) {
          gLastDisconnectReason = info.wifi_sta_disconnected.reason;
        },
        ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    gDisconnectHandlerRegistered = true;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid);
  WiFi.AP.enableDhcpCaptivePortal();
  configureRoutes();
  dns_.start(53, "*", WiFi.softAPIP());
  server_.begin();
  active_ = true;
  // One scan up front, so the list is ready when the phone opens the page.
  startScan();
}

void SetupPortal::stop() {
  if (!active_) {
    return;
  }
  server_.stop();
  dns_.stop();
  if (scanning_) {
    WiFi.scanDelete();
    scanning_ = false;
  }
  active_ = false;
}

void SetupPortal::service() {
  if (!active_) {
    return;
  }
  dns_.processNextRequest();
  server_.handleClient();
  if (scanning_) {
    collectScan();
  }
  serviceTest();
  if (restartAtMs_ != 0 && static_cast<int32_t>(millis() - restartAtMs_) >= 0) {
    Serial.println(F("setup_portal_restart"));
    delay(50);
    ESP.restart();
  }
}

void SetupPortal::configureRoutes() {
  if (routesConfigured_) {
    return;
  }
  server_.on("/", HTTP_GET, [this]() { handlePage(); });
  server_.on("/portal", HTTP_GET, [this]() { handlePage(); });
  server_.on("/hotspot-detect.html", HTTP_GET, [this]() { handlePage(); });
  server_.on("/api/info", HTTP_GET, [this]() { handleInfo(); });
  server_.on("/api/scan", HTTP_GET, [this]() { handleScan(); });
  server_.on("/api/save", HTTP_POST, [this]() { handleSave(); });
  server_.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  server_.on("/api/reset", HTTP_POST, [this]() { handleReset(); });
  // Every other captive-portal probe (generate_204, connecttest.txt, ...).
  server_.onNotFound([this]() { handleRedirect(); });
  routesConfigured_ = true;
}

void SetupPortal::sendJson(int code, const String& body) {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(code, "application/json", body);
}

void SetupPortal::handlePage() {
  server_.sendHeader("Content-Encoding", "gzip");
  server_.sendHeader("Cache-Control", "no-store");
  server_.send_P(200, "text/html", reinterpret_cast<PGM_P>(kSetupPortalGz), sizeof(kSetupPortalGz));
}

void SetupPortal::handleRedirect() {
  server_.sendHeader("Location", String("http://") + domain_ + "/portal", true);
  server_.send(302, "text/plain", "");
}

void SetupPortal::handleInfo() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macText[18];
  snprintf(macText, sizeof(macText), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  String out = "{";
  bool first = true;
  jsonStringField(out, "product", identity_.product, first);
  jsonStringField(out, "variant", identity_.variant, first);
  jsonStringField(out, "board", identity_.board, first);
  jsonStringField(out, "version", identity_.version, first);
  jsonStringField(out, "ap_ssid", apSsid_, first);
  jsonStringField(out, "domain", domain_, first);
  jsonStringField(out, "ip", WiFi.softAPIP().toString(), first);
  jsonStringField(out, "mac", macText, first);
  jsonBoolField(out, "can_reset", static_cast<bool>(hooks_.factoryReset), first);
  if (hooks_.info) {
    hooks_.info(out);
  }
  out += '}';
  sendJson(200, out);
}

void SetupPortal::handleScan() {
  if (server_.arg("refresh") == "1" && millis() - lastScanMs_ >= kRescanMinIntervalMs) {
    startScan();
  }
  sendJson(200, String(wifiScanJson(networks_, scanning_).c_str()));
}

void SetupPortal::startScan() {
  if (scanning_ || testState_ == TestState::Connecting) {
    return;
  }
  lastScanMs_ = millis();
  const int16_t result = WiFi.scanNetworks(true, false, false, kScanMsPerChannel);
  scanning_ = result == WIFI_SCAN_RUNNING;
}

void SetupPortal::collectScan() {
  const int16_t count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING) {
    return;
  }
  scanning_ = false;
  if (count < 0) {
    return;  // failed: keep the previous list
  }
  std::vector<WifiScanEntry> raw;
  raw.reserve(count);
  for (int16_t i = 0; i < count; ++i) {
    WifiScanEntry entry;
    entry.ssid = WiFi.SSID(i).c_str();
    entry.rssi = WiFi.RSSI(i);
    entry.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    raw.push_back(entry);
  }
  WiFi.scanDelete();
  networks_ = tidyWifiScan(raw, kMaxListedNetworks);
}

void SetupPortal::handleSave() {
  if (testState_ == TestState::Connecting || restartAtMs_ != 0) {
    sendJson(409, "{\"ok\":false,\"errors\":{\"form\":\"busy\"}}");
    return;
  }
  String errors;
  String ssid;
  String password;
  const SetupPortalSave result = hooks_.save ? hooks_.save(server_, errors, ssid, password)
                                             : SetupPortalSave::Invalid;
  if (result == SetupPortalSave::Invalid) {
    sendJson(400, String("{\"ok\":false,\"errors\":{") + errors + "}}");
    return;
  }
  if (result == SetupPortalSave::Restart) {
    restartAtMs_ = millis() + kRestartAfterSaveMs;
    sendJson(200, "{\"ok\":true,\"state\":\"restarting\"}");
    return;
  }
  if (scanning_) {
    WiFi.scanDelete();
    scanning_ = false;
  }
  testSsid_ = ssid;
  testPassword_ = password;
  failCode_ = "";
  warning_ = "";
  gLastDisconnectReason = 0;
  testStartMs_ = millis();
  testState_ = TestState::Connecting;
  Serial.print(F("setup_portal_test ssid="));
  Serial.println(ssid);
  WiFi.begin(ssid.c_str(), password.isEmpty() ? nullptr : password.c_str());
  sendJson(200, "{\"ok\":true,\"state\":\"connecting\"}");
}

void SetupPortal::serviceTest() {
  if (testState_ != TestState::Connecting) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("setup_portal_test_ok ip="));
    Serial.println(WiFi.localIP());
    if (hooks_.probe) {
      const char* warning = hooks_.probe();
      warning_ = warning != nullptr ? warning : "";
    }
    if (hooks_.onConnected) {
      hooks_.onConnected(testSsid_, testPassword_);
    }
    testPassword_ = "";
    testState_ = TestState::Ok;
    restartAtMs_ = millis() + kRestartAfterOkMs;
    return;
  }
  const uint32_t elapsed = millis() - testStartMs_;
  const uint8_t reason = gLastDisconnectReason;
  const char* code = staConnectFailureCode(reason);
  const bool definite = reason != 0 && (code == kStaFailWrongPassword || code == kStaFailNotFound);
  if ((definite && elapsed >= kTestEarlyFailMs) || elapsed >= kTestTimeoutMs) {
    failCode_ = code;
    testPassword_ = "";
    testState_ = TestState::Failed;
    // Stop retrying in the background; the AP stays up.
    WiFi.disconnect(false, false);
    Serial.print(F("setup_portal_test_failed reason="));
    Serial.print(reason);
    Serial.print(' ');
    Serial.println(code);
  }
}

void SetupPortal::handleStatus() {
  String out = "{";
  bool first = true;
  jsonStringField(out, "state", testStateName(static_cast<uint8_t>(testState_)), first);
  jsonStringField(out, "ssid", testSsid_, first);
  jsonStringField(out, "reason", failCode_, first);
  jsonStringField(out, "warning", warning_, first);
  out += '}';
  sendJson(200, out);
}

void SetupPortal::handleReset() {
  if (!hooks_.factoryReset) {
    sendJson(404, "{\"ok\":false}");
    return;
  }
  hooks_.factoryReset();
  restartAtMs_ = millis() + kRestartAfterSaveMs;
  sendJson(200, "{\"ok\":true,\"state\":\"restarting\"}");
}

}  // namespace nhos
