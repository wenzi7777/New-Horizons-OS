// Phase 0 spike (see plan: gcu-v2-3-d-new-horizons-hub-ble-hub-hub-lazy-marshmallow.md,
// section 6.1): measures real-world ESP-NOW throughput/loss when streaming
// synthetic frames sized like the worst-case NHO/Arduino/1 packet, at the
// current default streaming rate, to see whether "60fps / 2-4 concurrent
// devices" is actually achievable before any product firmware is written.
//
// Flash this sketch to 1-4 boards (one per simulated device). Flash
// espnow_throughput_receiver to a separate board first and copy its
// printed MAC address into kReceiverMac below before flashing senders.
// Every board runs IDENTICAL firmware -- no per-board slot config needed
// (see kHelloMagic/kPollMagic below): each device announces itself once
// at boot, then only transmits when the Hub explicitly polls it.
//
// EspNowFrame.h/.cpp here are a copy of
// firmware/newhorizons_os/EspNowFrame.{h,cpp} -- this spike sketch is
// throwaway validation code, kept dependency-free from the rest of the
// firmware tree so it builds as a standalone Arduino sketch. Keep in sync
// by hand if EspNowFrame changes before the spike is re-run.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <cstring>

#include "EspNowFrame.h"

namespace {

// ---- Spike config: edit before flashing ----
constexpr uint8_t kEspNowChannel = 6;   // must match the receiver
// PHY rate config (2026-08-06): the fps ceiling recorded below was
// measured entirely at ESP-NOW's default WIFI_PHY_RATE_1M_L (1Mbps, 802.11b
// long preamble). MCS3 LGI (~26Mbps) validated clean at both 24fps and
// 40fps target, usable (<2% loss) at ~6m through a wall on battery power --
// see the README's "PHY rate config" section for the full results table.
// This is the value now wired into production (EspNowPairing.cpp,
// EspNowHubManager.cpp).
constexpr wifi_phy_mode_t kEspNowPeerPhyMode = WIFI_PHY_MODE_HT20;
constexpr wifi_phy_rate_t kEspNowPeerPhyRate = WIFI_PHY_RATE_MCS3_LGI;
// Measured on real hardware (VD-CTL/R v1.0.F sender -> VD-CTL/R v2.3.D GCU
// LTS receiver, single link, this 1884B/8-fragment payload): 20fps is
// clean (0% loss, ~300kbit/s sustained); 40fps and 60fps both collapse
// (60-90%+ loss) -- the real per-link ceiling at this payload size is
// somewhere around 20-25fps, well below the Config.h kDefaultTargetFps=60
// this spike is meant to validate against. See the plan's Phase 0 section
// for the follow-up decision this implies.
constexpr uint16_t kTargetFps = 24;     // "Direct Stability Mode" real target -- see README's "PHY rate config" section
// "Direct Stability Mode" candidate size: same as kMaxPacketBytes but with
// the raw ADC block dropped (matrix levels + IMU + mag + battery + HMAC
// only) -- 1884 - (225 floats * 4B) = 984.
constexpr size_t kPayloadBytes = 984;
// Fill in from the receiver's boot-time Serial print.
uint8_t kReceiverMac[6] = {0xB8, 0xF8, 0x62, 0xC6, 0xF4, 0x24};

// Hub-driven unicast polling (plan section 6a, 2nd iteration -- see
// espnow_throughput_receiver.ino for the full story of why a broadcast
// sync beacon wasn't reliable enough). This device does nothing on its
// own timer at all: it announces itself once via HELLO, then only
// fragments+sends when it receives a POLL addressed to it. No per-board
// slot config -- identical firmware on every sender.
constexpr uint8_t kHelloMagic = 0xE1;
constexpr uint8_t kPollMagic = 0xE2;
constexpr uint32_t kFrameIntervalUs = 1000000UL / kTargetFps;
constexpr uint8_t kMaxPeers = 4;  // must match receiver kMaxPeers
constexpr uint32_t kPollBudgetUs = kFrameIntervalUs / kMaxPeers;
constexpr uint32_t kHelloRetryMs = 2000;
bool registeredWithHub = false;
uint32_t lastHelloMs = 0;
volatile bool pollPending = false;

uint8_t payload[kPayloadBytes];
uint16_t frameId = 0;

uint32_t framesSent = 0;
uint32_t fragmentsSent = 0;
uint32_t sendCallOk = 0;
uint32_t sendCallFail = 0;
volatile uint32_t sendCbOk = 0;
volatile uint32_t sendCbFail = 0;
uint32_t lastReportMs = 0;

// Pacing state: spreading a frame's fragments evenly across this device's
// poll budget (instead of firing all of them back-to-back) turned out to
// be necessary -- see the loop() comment below for what an un-paced burst
// did to ESP-NOW's internal TX queue.
nhos::EspNowFragment pendingFrags[nhos::kEspNowMaxFragCount];
uint8_t pendingCount = 0;
uint8_t pendingSent = 0;
uint32_t nextFragDueUs = 0;
uint32_t fragIntervalUs = 0;

void onSent(const esp_now_send_info_t* /*info*/, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    sendCbOk++;
  } else {
    sendCbFail++;
  }
}

void onRecv(const esp_now_recv_info_t* /*info*/, const uint8_t* data, int len) {
  if (len == 1 && data[0] == kPollMagic) {
    registeredWithHub = true;
    pollPending = true;
  }
}

void sendHello() {
  const uint8_t payload8[1] = {kHelloMagic};
  esp_now_send(kReceiverMac, payload8, sizeof(payload8));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  for (size_t i = 0; i < kPayloadBytes; ++i) {
    payload[i] = static_cast<uint8_t>(i);
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(kEspNowChannel, WIFI_SECOND_CHAN_NONE);
  // Defensive: HT rates need 802.11n enabled on this interface. STA mode's
  // default protocol bitmask is expected to already include it, but that's
  // never been confirmed on this hardware/core -- set it explicitly rather
  // than find out via a mystifying rate-config failure.
  {
    const esp_err_t protoErr = esp_wifi_set_protocol(
        WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    Serial.printf("[sender] esp_wifi_set_protocol -> %d\n", protoErr);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("[sender] esp_now_init FAILED");
    while (true) delay(1000);
  }
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kReceiverMac, 6);
  peer.channel = kEspNowChannel;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[sender] esp_now_add_peer FAILED -- check kReceiverMac");
  } else {
    esp_now_rate_config_t rateConfig = {};
    rateConfig.phymode = kEspNowPeerPhyMode;
    rateConfig.rate = kEspNowPeerPhyRate;
    rateConfig.ersu = false;
    rateConfig.dcm = false;
    const esp_err_t rateErr =
        esp_now_set_peer_rate_config(kReceiverMac, &rateConfig);
    Serial.printf("[sender] esp_now_set_peer_rate_config -> %d\n", rateErr);
  }

  // See espnow_throughput_receiver.ino -- WiFi.macAddress() was unreliable
  // on this core/board, read straight from the ESP-IDF API instead.
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  Serial.printf(
      "[sender] my_mac=%02X:%02X:%02X:%02X:%02X:%02X payload=%uB "
      "target_fps=%u frag_max=%uB frags_per_frame=%u poll_budget_us=%u\n",
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
      static_cast<unsigned>(kPayloadBytes), kTargetFps,
      static_cast<unsigned>(nhos::kEspNowFragMaxPayload),
      static_cast<unsigned>((kPayloadBytes + nhos::kEspNowFragMaxPayload - 1) /
                             nhos::kEspNowFragMaxPayload),
      static_cast<unsigned>(kPollBudgetUs));

  sendHello();
  lastHelloMs = millis();
}

void loop() {
  const uint32_t now = millis();
  const uint32_t nowUs = micros();

  // Keep re-announcing periodically even once registered -- if the Hub
  // restarts (as happened repeatedly in this spike, triggered by opening
  // its serial port to read stats) it forgets its whole peer roster, but
  // this device has no way to know that happened unless it keeps
  // reminding the Hub it exists.
  if (now - lastHelloMs >= kHelloRetryMs) {
    sendHello();
    lastHelloMs = now;
  }

  // A poll arriving is this device's *only* trigger to start a new frame
  // -- there is no local timer driving frame cadence at all anymore.
  // Originally fragments were fired all-back-to-back in one loop()
  // iteration, which saturated ESP-NOW's internal TX queue immediately
  // (observed ~60% of esp_now_send() calls returning
  // ESP_ERR_ESPNOW_NO_MEM even with a short retry loop) -- fixed by
  // spacing fragments evenly across the poll budget instead.
  if (pendingSent >= pendingCount && pollPending) {
    pollPending = false;
    pendingCount = nhos::EspNowFragmenter::fragment(
        payload, kPayloadBytes, frameId++, nhos::kEspNowFragTypeData,
        pendingFrags, nhos::kEspNowMaxFragCount);
    pendingSent = 0;
    fragIntervalUs =
        pendingCount > 0 ? kPollBudgetUs / pendingCount : 0;
    nextFragDueUs = nowUs;
    framesSent++;
  }

  if (pendingSent < pendingCount &&
      static_cast<int32_t>(nowUs - nextFragDueUs) >= 0) {
    const esp_err_t err = esp_now_send(
        kReceiverMac, pendingFrags[pendingSent].bytes,
        pendingFrags[pendingSent].len);
    if (err == ESP_OK) {
      sendCallOk++;
    } else {
      sendCallFail++;
    }
    fragmentsSent++;
    pendingSent++;
    nextFragDueUs += fragIntervalUs;
  }

  if (now - lastReportMs >= 5000) {
    lastReportMs = now;
    Serial.printf(
        "[sender] frames=%lu frags=%lu send_ok=%lu send_fail=%lu cb_ok=%lu "
        "cb_fail=%lu\n",
        static_cast<unsigned long>(framesSent),
        static_cast<unsigned long>(fragmentsSent),
        static_cast<unsigned long>(sendCallOk),
        static_cast<unsigned long>(sendCallFail),
        static_cast<unsigned long>(sendCbOk),
        static_cast<unsigned long>(sendCbFail));
  }
}
