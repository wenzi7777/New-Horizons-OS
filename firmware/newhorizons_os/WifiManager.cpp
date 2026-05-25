#include "WifiManager.h"

#include "Config.h"

#include <esp_mac.h>

namespace nhos {

bool WifiManager::begin(Storage& storage, bool forceSetupPortal) {
  storage_ = &storage;
  if (forceSetupPortal) {
    Serial.println(F("wifi_setup_requested_by_action_button"));
    startSetupAp();
    return false;
  }
  if (!hasCredentials()) {
    Serial.println(F("wifi_sta_no_credentials"));
    startSetupAp();
    return false;
  }
  if (connectStored()) {
    setupActive_ = false;
    Serial.print(F("wifi_sta_connected ip="));
    Serial.println(WiFi.localIP());
    return true;
  }
  startSetupAp();
  return false;
}

void WifiManager::service() {
  if (setupActive_) {
    serviceSetupPortal();
    return;
  }
  if (isConnected() || millis() - lastReconnectMs_ < kWifiReconnectMs) {
    return;
  }
  lastReconnectMs_ = millis();
  connectStored();
}

bool WifiManager::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

bool WifiManager::setupActive() const {
  return setupActive_;
}

bool WifiManager::hasCredentials() const {
  return storage_ && !storage_->getString("wifi_ssid", "").isEmpty();
}

bool WifiManager::applyCredentials(const String& ssid, const String& password) {
  if (!storage_ || ssid.isEmpty()) {
    return false;
  }
  storage_->putString("wifi_ssid", ssid);
  storage_->putString("wifi_pass", password);
  stopSetupPortal();
  WiFi.mode(WIFI_STA);
  bool connected = connectStored();
  if (!connected) {
    Serial.println(F("wifi_sta_connect_failed restarting_setup_ap"));
    startSetupAp();
  } else {
    Serial.print(F("wifi_sta_connected ip="));
    Serial.println(WiFi.localIP());
  }
  return connected;
}

void WifiManager::clearCredentials() {
  if (!storage_) {
    return;
  }
  storage_->putString("wifi_ssid", "");
  storage_->putString("wifi_pass", "");
  startSetupAp();
}

String WifiManager::statusJson() const {
  String out = "{";
  out += "\"connected\":";
  out += isConnected() ? "true" : "false";
  out += ",\"setup_active\":";
  out += setupActive_ ? "true" : "false";
  out += ",\"rssi\":";
  out += isConnected() ? WiFi.RSSI() : 0;
  out += ",\"ip\":\"";
  out += isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  out += "\"}";
  return out;
}

void WifiManager::macBytes(uint8_t out[6]) const {
  esp_read_mac(out, ESP_MAC_WIFI_STA);
}

bool WifiManager::connectStored() {
  if (!storage_) {
    return false;
  }
  if (!hasCredentials()) {
    Serial.println(F("wifi_sta_no_credentials"));
    return false;
  }
  const String ssid = storage_->getString("wifi_ssid", "");
  const String password = storage_->getString("wifi_pass", "");
  WiFi.mode(WIFI_STA);
  Serial.print(F("wifi_sta_connect_start ssid="));
  Serial.println(ssid);
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t start = millis();
  while (millis() - start < 8000) {
    if (isConnected()) {
      return true;
    }
    delay(50);
  }
  return false;
}

void WifiManager::startSetupAp() {
  stopSetupPortal();
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char ssid[48];
  snprintf(
      ssid,
      sizeof(ssid),
      "%s-%02X%02X%02X%02X%02X%02X",
      kDefaultApSsidPrefix,
      mac[0],
      mac[1],
      mac[2],
      mac[3],
      mac[4],
      mac[5]);
  Serial.println(F("wifi_setup_ap_starting"));
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid);
  WiFi.AP.enableDhcpCaptivePortal();
  configurePortalRoutes();
  dnsServer_.start(DNS_DEFAULT_PORT, "*", WiFi.softAPIP());
  portalServer_.begin();
  portalStarted_ = true;
  setupActive_ = true;
  Serial.print(F("wifi_setup_ap_started ssid="));
  Serial.print(ssid);
  Serial.print(F(" ip="));
  Serial.print(WiFi.softAPIP());
  Serial.print(F(" portal=http://"));
  Serial.println(kSetupPortalDomain);
}

void WifiManager::stopSetupPortal() {
  const bool apWasActive = portalStarted_ || setupActive_;
  if (portalStarted_) {
    portalServer_.stop();
    dnsServer_.stop();
    portalStarted_ = false;
  }
  setupActive_ = false;
  if (apWasActive) {
    WiFi.softAPdisconnect(true);
  }
}

void WifiManager::configurePortalRoutes() {
  if (portalRoutesConfigured_) {
    return;
  }
  portalServer_.on("/", HTTP_GET, [this]() {
    handlePortalRoot();
  });
  portalServer_.on("/portal", HTTP_GET, [this]() {
    handlePortalRoot();
  });
  portalServer_.on("/save", HTTP_POST, [this]() {
    handlePortalSave();
  });
  portalServer_.on("/generate_204", HTTP_GET, [this]() {
    handlePortalRedirect();
  });
  portalServer_.on("/gen_204", HTTP_GET, [this]() {
    handlePortalRedirect();
  });
  portalServer_.on("/hotspot-detect.html", HTTP_GET, [this]() {
    handlePortalRoot();
  });
  portalServer_.on("/connecttest.txt", HTTP_GET, [this]() {
    handlePortalRedirect();
  });
  portalServer_.on("/ncsi.txt", HTTP_GET, [this]() {
    handlePortalRedirect();
  });
  portalServer_.onNotFound([this]() {
    handlePortalRedirect();
  });
  portalRoutesConfigured_ = true;
}

void WifiManager::serviceSetupPortal() {
  if (!portalStarted_) {
    return;
  }
  dnsServer_.processNextRequest();
  portalServer_.handleClient();
}

void WifiManager::handlePortalRoot() {
  portalServer_.sendHeader("Cache-Control", "no-store");
  portalServer_.send(200, "text/html", portalPage("", false));
}

void WifiManager::handlePortalSave() {
  String ssid = portalServer_.arg("ssid");
  String password = portalServer_.arg("password");
  ssid.trim();
  if (ssid.isEmpty()) {
    portalServer_.sendHeader("Cache-Control", "no-store");
    portalServer_.send(400, "text/html", portalPage("SSID is required.", false));
    return;
  }

  portalServer_.sendHeader("Cache-Control", "no-store");
  portalServer_.send(200, "text/html", portalPage("Credentials saved. Connecting...", true));
  delay(150);
  applyCredentials(ssid, password);
}

void WifiManager::handlePortalRedirect() {
  portalServer_.sendHeader("Location", String("http://") + kSetupPortalDomain + "/portal", true);
  portalServer_.send(302, "text/plain", "redirect to New Horizons OS Wi-Fi setup");
}

String WifiManager::portalPage(const String& message, bool success) const {
  String savedSsid = storage_ ? storage_->getString("wifi_ssid", "") : "";
  String out;
  out.reserve(1800);
  out += F("<!doctype html><html><head><meta charset=\"utf-8\">");
  out += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  out += F("<title>New Horizons OS Wi-Fi Setup</title>");
  out += F("<style>body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;margin:0;background:#101417;color:#eef2f5}");
  out += F("main{max-width:440px;margin:0 auto;padding:28px 20px}h1{font-size:24px;margin:0 0 8px}");
  out += F("p{color:#b9c2ca;line-height:1.45}label{display:block;margin:16px 0 6px;color:#dce3e8}");
  out += F("input,select{box-sizing:border-box;width:100%;font-size:16px;padding:12px;border-radius:6px;border:1px solid #5f6b74;background:#151b20;color:#fff}");
  out += F("button{width:100%;margin-top:20px;padding:12px;font-size:16px;border:0;border-radius:6px;background:#2dd4bf;color:#041011;font-weight:700}");
  out += F(".msg{padding:10px 12px;border-radius:6px;background:#1f2930}.ok{background:#12382f}</style></head><body><main>");
  out += F("<h1>New Horizons OS Wi-Fi Setup</h1>");
  out += F("<p>Connect this board to your local Wi-Fi network.</p>");
  if (!message.isEmpty()) {
    out += success ? F("<p class=\"msg ok\">") : F("<p class=\"msg\">");
    out += htmlEscape(message);
    out += F("</p>");
  }
  out += F("<form method=\"post\" action=\"/save\">");
  out += F("<label for=\"ssid_select\">Nearby Wi-Fi</label>");
  out += F("<select id=\"ssid_select\" onchange=\"document.getElementById(\'ssid\').value=this.value\">");
  out += F("<option value=\"\">Select a network or type manually</option>");
  out += wifiNetworkOptionsHtml();
  out += F("</select>");
  out += F("<label for=\"ssid\">Wi-Fi SSID</label>");
  out += F("<input id=\"ssid\" name=\"ssid\" autocomplete=\"off\" value=\"");
  out += htmlEscape(savedSsid);
  out += F("\" required>");
  out += F("<label for=\"password\">Password</label>");
  out += F("<input id=\"password\" name=\"password\" type=\"password\" autocomplete=\"current-password\">");
  out += F("<button type=\"submit\">Connect</button>");
  out += F("</form><p>Setup AP: ");
  out += htmlEscape(WiFi.softAPSSID());
  out += F("<br>Manual URL: http://");
  out += kSetupPortalDomain;
  out += F("<br>Fallback: http://");
  out += WiFi.softAPIP().toString();
  out += F("</p></main></body></html>");
  return out;
}

String WifiManager::wifiNetworkOptionsHtml() const {
  String out;
  int16_t count = WiFi.scanNetworks(false, true, false, 160);
  if (count <= 0) {
    out += F("<option value=\"\">No networks found</option>");
    WiFi.scanDelete();
    return out;
  }
  for (int16_t i = 0; i < count && i < 16; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;
    }
    out += F("<option value=\"");
    out += htmlEscape(ssid);
    out += F("\">");
    out += htmlEscape(ssid);
    out += F(" (");
    out += WiFi.RSSI(i);
    out += F(" dBm, ");
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
      out += F("open");
    } else {
      out += F("secure");
    }
    out += F(")</option>");
  }
  WiFi.scanDelete();
  return out;
}

String WifiManager::htmlEscape(const String& value) const {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value.charAt(i);
    if (c == '&') {
      out += F("&amp;");
    } else if (c == '<') {
      out += F("&lt;");
    } else if (c == '>') {
      out += F("&gt;");
    } else if (c == '"') {
      out += F("&quot;");
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace nhos
