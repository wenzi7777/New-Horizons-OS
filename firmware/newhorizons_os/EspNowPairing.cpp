#include "EspNowPairing.h"

#include <cstring>
#include <esp_now.h>
#include <esp_wifi.h>

#include "ControlServer.h"
#include "EspNowOtaReceiver.h"

namespace nhos {

namespace {
// Offset of the frame-type byte within an EspNowFrame fragment header --
// MUST match New-Horizons-Hub/newhorizons_hub/EspNowHubManager.cpp's own
// kEspNowFragTypeOffset. Peeked before feeding a fragment to a reassembler
// so control/OTA/hub-request traffic never shares one (see
// EspNowPairing.h's otaReassembler_/hubRequestReassembler_ comment).
constexpr size_t kEspNowFragTypeOffset = 2;

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

EspNowPairing::EspNowPairing()
    : controlReassembler_(controlScratch_, sizeof(controlScratch_)),
      otaReassembler_(otaScratch_, sizeof(otaScratch_)),
      hubRequestReassembler_(hubRequestScratch_, sizeof(hubRequestScratch_)) {}

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

  // Missing/invalid hub_mac doesn't fail begin() anymore -- it means this
  // device has never been reachable via a Gateway to receive a
  // set_transport command, so it falls back to broadcast discovery
  // instead (serviceDiscovery()). See EspNowPairing.h's file comment.
  discovering_ = !parseHubMacFromConfig();
  // Unconditional (not just inside the discovering_ branch below) so the
  // shared kEspNowPairingTimeoutMs watchdog in service() covers both the
  // empty-hub_mac discovery path and the known-but-unreachable-hub_mac
  // path with one mechanism.
  pairingAttemptStartMs_ = millis();

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

  if (discovering_) {
    esp_now_peer_info_t bcastPeer = {};
    memcpy(bcastPeer.peer_addr, kEspNowBroadcastMac, 6);
    bcastPeer.channel = 0;
    bcastPeer.ifidx = WIFI_IF_STA;
    bcastPeer.encrypt = false;
    if (esp_now_add_peer(&bcastPeer) != ESP_OK) {
      Serial.println("[espnow_pairing] broadcast peer add FAILED");
      return false;
    }
    Serial.println("[espnow_pairing] no hub_mac configured, starting broadcast discovery");
  } else {
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
  if (otaReceiver_ != nullptr) {
    otaReceiver_->begin(*deviceConfig_, hubMac_);
    otaReceiver_->startBootCheck();
  }
}

void EspNowPairing::service() {
  serviceCommand();
  if (hubRequestFrameReady_) {
    hubRequestFrameReady_ = false;
    if (otaReceiver_ != nullptr) {
      otaReceiver_->handleHubRequestFrame(hubRequestFrameBuffer_, hubRequestFrameBufferLen_);
    }
  }
  if (otaFrameReady_) {
    otaFrameReady_ = false;
    if (otaReceiver_ != nullptr) {
      otaReceiver_->handleOtaChunkFrame(otaFrameBuffer_, otaFrameBufferLen_);
    }
  }
  // Unified "hasn't paired since begin()" watchdog -- covers both
  // discovering_ (empty hub_mac) and the known-but-unreachable-hub_mac
  // case (neither of which has its own separate timeout anymore). Checked
  // before discovering_'s own branch so a timed-out discovery attempt
  // never reaches serviceDiscovery() at all this tick.
  if (!paired_ && millis() - pairingAttemptStartMs_ >= kEspNowPairingTimeoutMs) {
    failPairingAndRestart(discovering_ ? "discovery_timeout" : "known_hub_unreachable_timeout");
    return;
  }
  if (discovering_) {
    serviceDiscovery();
    return;
  }
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
  // Checked first, before the hubMac_ filter below: discovering_ can only
  // be true pre-boot-decision (set once in begin(), from an empty/invalid
  // configured hub_mac) and is mutually exclusive with hubMacValid_ for
  // the device's entire boot lifetime -- an already-paired device's
  // routine keepalive HELLO/PAIRED exchange with its own Hub never
  // reaches this branch, so it can't be misread as "found a new Hub".
  if (discovering_) {
    if (len == 1 && data[0] == kEspNowPairedMagic) {
      onHubDiscovered(mac);
    }
    return;  // no POLL/control traffic is meaningful before a hub_mac exists
  }
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
    handleFragmentedPacket(data, len);
  }
}

void EspNowPairing::serviceDiscovery() {
  const uint32_t now = millis();
  startScanIfNeeded();  // same channel-hop as the known-hub_mac path
  if (now - lastHelloMs_ >= kEspNowHelloRetryMs) {
    sendDiscoveryHello();
    lastHelloMs_ = now;
  }
}

void EspNowPairing::failPairingAndRestart(const char* reason) {
  // No live in-place fallback here on purpose -- mirrors
  // ControlServer.cpp's set_transport handler, which "deliberately does
  // not attempt a live in-place transport swap, which would need to
  // tear down/reinit WiFiUDP or ESP-NOW mid-loop." newhorizons_os.ino's
  // setup() reads this flag on the *next* boot and forces the WiFi
  // SoftAP portal instead of retrying a doomed pairing attempt.
  Serial.print("[espnow_pairing] pairing_timeout reason=");
  Serial.println(reason);
  if (storage_) {
    storage_->putUInt(kEspNowPairingFailFlagKey, 1);
  }
  delay(50);
  ESP.restart();
}

void EspNowPairing::sendDiscoveryHello() {
  const uint8_t payload[1] = {kEspNowHelloMagic};
  esp_now_send(kEspNowBroadcastMac, payload, sizeof(payload));
}

void EspNowPairing::onHubDiscovered(const uint8_t mac[6]) {
  if (!discovering_) return;  // guard against a stray duplicate callback before restart lands
  discovering_ = false;
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  Serial.print("[espnow_pairing] discovered hub ");
  Serial.println(macStr);
  deviceConfig_->setTransport("espnow", String(macStr));
  if (storage_) {
    storage_->putUInt(kEspNowPairingFailFlagKey, 0);  // clear any stale failure flag
    deviceConfig_->save(*storage_);
  }
  delay(50);
  ESP.restart();
}

void EspNowPairing::handleFragmentedPacket(const uint8_t* data, size_t len) {
  // Runs on the ESP-NOW/WiFi driver task (small stack) -- keep every branch
  // here to reassembly + a memcpy only. Peek the frame-type byte before
  // feeding any reassembler, exactly like EspNowHubManager.cpp's own
  // 3-way routing -- see otaReassembler_/hubRequestReassembler_'s comment
  // in the header for why these can never share controlReassembler_.
  const uint8_t frameTypeByte =
      len > kEspNowFragTypeOffset ? data[kEspNowFragTypeOffset] : kEspNowFragTypeData;

  if (frameTypeByte == kEspNowFragTypeOta) {
    ReassembledFrame frame;
    if (!otaReassembler_.onFragment(data, len, &frame)) return;
    if (frame.len > sizeof(otaFrameBuffer_)) return;
    memcpy(otaFrameBuffer_, frame.data, frame.len);
    otaFrameBufferLen_ = frame.len;
    otaFrameReady_ = true;
    return;
  }
  if (frameTypeByte == kEspNowFragTypeHubRequest) {
    ReassembledFrame frame;
    if (!hubRequestReassembler_.onFragment(data, len, &frame)) return;
    if (frame.len > sizeof(hubRequestFrameBuffer_)) return;
    memcpy(hubRequestFrameBuffer_, frame.data, frame.len);
    hubRequestFrameBufferLen_ = frame.len;
    hubRequestFrameReady_ = true;
    return;
  }

  // Default: kEspNowFragTypeControl (existing behavior, unchanged). See
  // pendingCommandBuffer_'s comment for why actual command processing is
  // deferred to serviceCommand().
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
