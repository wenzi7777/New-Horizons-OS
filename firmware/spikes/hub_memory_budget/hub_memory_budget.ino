// Phase 0 spike (see plan: gcu-v2-3-d-new-horizons-hub-ble-hub-hub-lazy-marshmallow.md,
// section 6.2): measures heap headroom when WiFi STA + a TLS WebSocket
// client + ESP-NOW (with 4 registered peers, matching the plan's 2-4
// concurrent-device target) all run at once on a no-PSRAM ESP32-S3 (GCU
// V2.3.D hardware, or any ESP32-S3 dev board for a first pass). This is
// the biggest open memory-budget risk called out in the plan -- do not
// assume it fits; this spike exists to produce real numbers.
//
// Requires the "WebSockets" library by Markus Sattler
// (https://github.com/Links2004/arduinoWebSockets, installable via
// `arduino-cli lib install WebSockets` or Library Manager).
//
// Edit the config block below, flash, then watch Serial at 115200 baud.
// Compare the reported free-heap numbers against
// newhorizons_os.ino's own `ESP.getFreeHeap() < 30000` danger threshold
// (see NewHorizonsOS-OTA/firmware/newhorizons_os/newhorizons_os.ino) as a
// reference for what "tight but survivable" looks like on this exact
// SoC/flash configuration.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <WebSocketsClient.h>

#include <cstring>

#include "EspNowFrame.h"

namespace {

// ---- Spike config: edit before flashing ----
constexpr char kWifiSsid[] = "CNLab-wifi";
constexpr char kWifiPassword[] = "uoacnlab2023";

// Point this at either a local Desktop Backend instance
// (`./scripts/start_local.sh --build`, target_mode=local, ws:// no TLS) for
// a baseline, or the production endpoint (wss://, exercises the TLS
// handshake this spike primarily cares about). Matches
// New-Horizons-Gateway/newhorizons_gateway/config_store.py's target_mode
// URL shapes.
constexpr bool kUseTls = true;
constexpr char kWsHost[] = "isensing-s1.u-aizu.ac.jp";
constexpr uint16_t kWsPort = 443;
constexpr char kWsPath[] = "/newhorizons/gateway/ws";

constexpr uint8_t kEspNowChannel = 6;
constexpr uint8_t kSimulatedPeerCount = 4;  // matches plan's 2-4 device target
constexpr size_t kReassemblyScratchBytes =
    nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload;

WebSocketsClient webSocket;

// Static allocation matching what the real Hub firmware would need: one
// reassembly buffer per paired device slot. Allocated regardless of
// whether ESP-NOW traffic actually flows in this spike -- the point is to
// measure the static/BSS cost, which is paid whether or not the slots are
// in active use.
uint8_t reassemblyScratch[kSimulatedPeerCount][kReassemblyScratchBytes];

uint32_t lastHeapLogMs = 0;
bool loggedWifiConnected = false;
bool loggedEspNowReady = false;

void logHeap(const char* stage) {
  Serial.printf(
      "[heap] stage=%-24s free=%u max_alloc=%u min_free_ever=%u\n", stage,
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMaxAllocHeap()),
      static_cast<unsigned>(ESP.getMinFreeHeap()));
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[ws] connected");
      logHeap("ws_connected");
      // Mirrors New-Horizons-Gateway/newhorizons_gateway/upstream_wss.py's
      // hello handshake shape closely enough to exercise a realistic
      // send-path allocation, without needing a full HubUplinkClient.
      webSocket.sendTXT(
          "{\"type\":\"hello\",\"gateway_id\":\"spike-hub\",\"role\":\"hub\"}");
      break;
    case WStype_DISCONNECTED:
      Serial.println("[ws] disconnected");
      logHeap("ws_disconnected");
      break;
    case WStype_ERROR:
      Serial.printf("[ws] error, len=%u\n", static_cast<unsigned>(length));
      logHeap("ws_error");
      break;
    case WStype_TEXT:
      Serial.printf("[ws] rx text (%u bytes)\n", static_cast<unsigned>(length));
      break;
    default:
      break;
  }
  (void)payload;
}

void bringUpEspNowPeers() {
  for (uint8_t i = 0; i < kSimulatedPeerCount; ++i) {
    esp_now_peer_info_t peer = {};
    // Dummy locally-administered MAC addresses -- no real device on the
    // other end, this is purely to measure the peer table's memory cost.
    peer.peer_addr[0] = 0x02;
    peer.peer_addr[1] = 0x00;
    peer.peer_addr[2] = 0x00;
    peer.peer_addr[3] = 0x00;
    peer.peer_addr[4] = 0x00;
    peer.peer_addr[5] = i;
    peer.channel = kEspNowChannel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    const esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
      Serial.printf("[espnow] add_peer %u failed: %d\n", i, static_cast<int>(err));
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  logHeap("boot");

  WiFi.mode(WIFI_STA);
  WiFi.begin(kWifiSsid, kWifiPassword);
  Serial.printf("[wifi] connecting to %s ...\n", kWifiSsid);
}

void loop() {
  const uint32_t now = millis();

  if (!loggedWifiConnected && WiFi.status() == WL_CONNECTED) {
    loggedWifiConnected = true;
    Serial.printf("[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());
    logHeap("wifi_connected");

    esp_wifi_set_channel(kEspNowChannel, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
      Serial.println("[espnow] init FAILED");
    } else {
      bringUpEspNowPeers();
      loggedEspNowReady = true;
      logHeap("espnow_ready_4peers");
    }

    webSocket.onEvent(onWsEvent);
    webSocket.setReconnectInterval(5000);
    if (kUseTls) {
      // No certificate pinning/validation -- this spike only cares about
      // the TLS handshake's heap cost, not connection security. The real
      // HubUplinkClient must validate the server certificate.
      webSocket.beginSSL(kWsHost, kWsPort, kWsPath);
    } else {
      webSocket.begin(kWsHost, kWsPort, kWsPath);
    }
    logHeap("ws_begin_called");
  }

  if (loggedWifiConnected) {
    webSocket.loop();
  }

  if (now - lastHeapLogMs >= 1000) {
    lastHeapLogMs = now;
    logHeap("steady_state_poll");
  }

  (void)loggedEspNowReady;
}
