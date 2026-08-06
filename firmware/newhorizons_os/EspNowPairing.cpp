#include "EspNowPairing.h"

#include <cstring>
#include <esp_now.h>
#include <esp_wifi.h>

#include "ControlServer.h"

namespace nhos {

namespace {
// Mirrors EspNowStreamTransport.cpp's kSendWindowUs -- same reasoning
// (spread a multi-fragment burst out so esp_now_send()'s internal queue
// isn't overwhelmed, see EspNowPairing.h's responseFrags_ comment).
constexpr uint32_t kEspNowResponseSendWindowUs = 15000;

// PHY rate for the ESP-NOW link to the Hub. ESP-NOW defaults to
// WIFI_PHY_RATE_1M_L (1Mbps, 802.11b long preamble) -- the previously
// recorded fps ceiling (see firmware/spikes/README.md) was entirely
// airtime-bound at that default rate. Real-hardware validated 2026-08-06
// (same README, "PHY rate" section): MCS3 LGI (~26Mbps) held loss=0.0%
// clean at both 24fps and 40fps target (well past the old ~20-25fps
// ceiling), stayed usable (<2% loss) at ~6m through a wall on battery
// power. Non-fatal if unsupported -- falls back to the default rate.
constexpr wifi_phy_mode_t kEspNowPeerPhyMode = WIFI_PHY_MODE_HT20;
constexpr wifi_phy_rate_t kEspNowPeerPhyRate = WIFI_PHY_RATE_MCS3_LGI;

bool parseMac(const String& text, uint8_t out[6]) {
  if (text.length() != 17) return false;
  uint8_t bytes[6] = {0};
  for (int i = 0; i < 6; ++i) {
    const char hi = text.charAt(i * 3);
    const char lo = text.charAt(i * 3 + 1);
    if (i < 5 && text.charAt(i * 3 + 2) != ':') return false;
    if (!isHexadecimalDigit(hi) || !isHexadecimalDigit(lo)) return false;
    auto nibble = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
      return static_cast<uint8_t>(c - 'A' + 10);
    };
    bytes[i] = static_cast<uint8_t>((nibble(hi) << 4) | nibble(lo));
  }
  memcpy(out, bytes, 6);
  return true;
}

}  // namespace

EspNowPairing::EspNowPairing() : controlReassembler_(controlScratch_, sizeof(controlScratch_)) {}

bool EspNowPairing::parseHubMacFromConfig() {
  const String& mac = deviceConfig_->data().transport.hubMac;
  hubMacValid_ = parseMac(mac, hubMac_);
  return hubMacValid_;
}

void EspNowPairing::buildScanChannelList() {
  scanChannelCount_ = 0;
  const uint8_t lastKnown = deviceConfig_->data().transport.lastKnownChannel;
  if (lastKnown != 0) {
    scanChannels_[scanChannelCount_++] = lastKnown;
  }
  for (uint8_t c : kEspNowScanChannels) {
    if (c == lastKnown) continue;
    scanChannels_[scanChannelCount_++] = c;
  }
  scanIndex_ = 0;
}

bool EspNowPairing::begin(Storage& storage, DeviceConfig& deviceConfig, ControlServer& control,
                           const uint8_t uid[6]) {
  storage_ = &storage;
  deviceConfig_ = &deviceConfig;
  control_ = &control;
  memcpy(uid_, uid, 6);

  if (!parseHubMacFromConfig()) {
    Serial.println("[espnow_pairing] invalid/missing hub_mac in config");
    return false;
  }

  // Defensive: HT rates need 802.11n enabled on this interface. STA mode's
  // default protocol bitmask is expected to already include it, but set it
  // explicitly rather than have the rate-config call below fail for an
  // unobvious reason.
  const esp_err_t protoErr = esp_wifi_set_protocol(
      WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  if (protoErr != ESP_OK) {
    Serial.printf("[espnow_pairing] esp_wifi_set_protocol -> %d\n", protoErr);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("[espnow_pairing] esp_now_init FAILED");
    return false;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, hubMac_, 6);
  peer.channel = 0;  // follow whatever channel this radio is currently on
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[espnow_pairing] esp_now_add_peer FAILED");
    return false;
  }

  esp_now_rate_config_t rateConfig = {};
  rateConfig.phymode = kEspNowPeerPhyMode;
  rateConfig.rate = kEspNowPeerPhyRate;
  rateConfig.ersu = false;
  rateConfig.dcm = false;
  const esp_err_t rateErr = esp_now_set_peer_rate_config(hubMac_, &rateConfig);
  if (rateErr != ESP_OK) {
    // Non-fatal by design -- falls back to the default (slow but
    // maximally compatible) ESP-NOW rate rather than blocking pairing.
    Serial.printf("[espnow_pairing] esp_now_set_peer_rate_config -> %d, using default rate\n",
                  rateErr);
  }

  buildScanChannelList();
  return true;
}

void EspNowPairing::startScanIfNeeded() {
  if (paired_) return;
  const uint32_t now = millis();
  if (now - lastChannelSwitchMs_ < kEspNowChannelDwellMs) return;
  lastChannelSwitchMs_ = now;
  const uint8_t channel = scanChannels_[scanIndex_];
  scanIndex_ = static_cast<uint8_t>((scanIndex_ + 1) % scanChannelCount_);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  lastHelloMs_ = 0;  // force an immediate HELLO on the new channel
}

void EspNowPairing::sendHello() {
  const uint8_t payload[1] = {kEspNowHelloMagic};
  esp_now_send(hubMac_, payload, sizeof(payload));
}

void EspNowPairing::onPaired() {
  if (paired_) return;
  paired_ = true;
  uint8_t currentChannel = 0;
  wifi_second_chan_t secondChannel = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&currentChannel, &secondChannel);
  deviceConfig_->setTransportLastKnownChannel(currentChannel);
  if (storage_) {
    deviceConfig_->save(*storage_);
  }
  Serial.println("[espnow_pairing] paired");
}

void EspNowPairing::service() {
  serviceCommand();
  if (!hubMacValid_) return;
  if (!paired_) {
    startScanIfNeeded();
    const uint32_t now = millis();
    if (now - lastHelloMs_ >= kEspNowHelloRetryMs) {
      sendHello();
      lastHelloMs_ = now;
    }
    return;
  }
  // Paired: keep re-announcing periodically in the background. Observed
  // during spike testing: if the Hub restarts, it forgets its peer
  // roster, but this device has no way to know that happened unless it
  // keeps reminding the Hub it exists (see
  // firmware/spikes/README.md's "Hub-driven unicast polling" section).
  const uint32_t now = millis();
  if (now - lastHelloMs_ >= kEspNowHelloRetryMs * 4) {
    sendHello();
    lastHelloMs_ = now;
  }
}

void EspNowPairing::handleEspNowRecv(const uint8_t mac[6], const uint8_t* data, size_t len) {
  if (!hubMacValid_ || memcmp(mac, hubMac_, 6) != 0) {
    return;  // ignore anything not from our configured Hub
  }
  if (len == 1 && data[0] == kEspNowPairedMagic) {
    onPaired();
    return;
  }
  if (len == 1 && data[0] == kEspNowPollMagic) {
    if (paired_) {
      pollPending_ = true;
    }
    return;
  }
  if (paired_) {
    handleControlFragment(data, len);
  }
}

void EspNowPairing::handleControlFragment(const uint8_t* data, size_t len) {
  // Runs on the ESP-NOW/WiFi driver task (small stack) -- keep this to
  // reassembly + a memcpy only. See pendingCommandBuffer_'s comment for
  // why actual command processing is deferred to serviceCommand().
  ReassembledFrame frame;
  if (!controlReassembler_.onFragment(data, len, &frame)) {
    return;
  }
  if (frame.frameType != kEspNowFragTypeControl || control_ == nullptr) {
    return;
  }
  if (frame.len > sizeof(pendingCommandBuffer_)) return;
  memcpy(pendingCommandBuffer_, frame.data, frame.len);
  pendingCommandLen_ = frame.len;
  commandPending_ = true;
}

void EspNowPairing::serviceCommand() {
  const uint32_t nowUs = micros();

  // Drain any in-progress paced response send first -- see
  // responseFrags_'s declaration comment for why this can't just loop
  // over esp_now_send() in one shot. Mirrors
  // EspNowStreamTransport::service()'s own pacing state machine.
  if (responseFragsSent_ < responseFragCount_) {
    if (static_cast<int32_t>(nowUs - responseNextFragDueUs_) >= 0) {
      esp_now_send(hubMac_, responseFrags_[responseFragsSent_].bytes,
                   responseFrags_[responseFragsSent_].len);
      ++responseFragsSent_;
      responseNextFragDueUs_ += responseFragIntervalUs_;
    }
    return;  // don't start processing a new command while still draining this response
  }

  if (!commandPending_ || control_ == nullptr) return;
  commandPending_ = false;

  const String response = control_->serviceEspNowCommand(pendingCommandBuffer_, pendingCommandLen_);
  if (response.isEmpty()) return;

  // Fragment the response back to the Hub; actual sending is paced out
  // across subsequent service() calls above. frameId=0 is fine here since
  // commands are processed synchronously and one-at-a-time -- there's
  // never a second response in flight to disambiguate against.
  responseFragCount_ = EspNowFragmenter::fragment(
      reinterpret_cast<const uint8_t*>(response.c_str()), response.length(), 0,
      kEspNowFragTypeControl, responseFrags_, kEspNowMaxFragCount);
  responseFragsSent_ = 0;
  responseFragIntervalUs_ = responseFragCount_ > 0 ? kEspNowResponseSendWindowUs / responseFragCount_ : 0;
  responseNextFragDueUs_ = nowUs;
}

bool EspNowPairing::consumePollPending() {
  if (!pollPending_) return false;
  pollPending_ = false;
  return true;
}

}  // namespace nhos
