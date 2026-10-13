#include "EspNowPairing.h"

#include "AirtimeArbiter.h"

#include <cstring>
#include <esp_now.h>
#include <new>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "ControlServer.h"
#include "EspNowOtaReceiver.h"
#include "JsonUtils.h"
#include "LifetimeStats.h"

namespace nhos {

namespace {
// Offset of the frame-type byte within an EspNowFrame fragment header --
// MUST match New-Horizons-Hub/newhorizons_hub/EspNowHubManager.cpp's own
// kEspNowFragTypeOffset. Peeked before feeding a fragment to a reassembler
// so control/OTA/hub-request traffic never shares one (see
// EspNowPairing.h's otaReassembler_/hubRequestReassembler_ comment).
constexpr size_t kEspNowFragTypeOffset = 2;

// A paged reply nobody asks for again within this long is dropped. The Hub
// re-asks a lost page every 500 ms, so a live transfer never goes quiet
// this long.
constexpr uint32_t kEspNowPagedResponseIdleMs = 10000;

// The Hub's page request is {"command":"result_page","rid":R,"chunk":I}.
constexpr char kPageRequestToken[] = "\"result_page\"";

bool bufferContains(const uint8_t* data, size_t len, const char* token) {
  const size_t tokenLen = strlen(token);
  if (tokenLen == 0 || len < tokenLen) return false;
  for (size_t i = 0; i + tokenLen <= len; ++i) {
    if (memcmp(data + i, token, tokenLen) == 0) return true;
  }
  return false;
}

// Stop-and-wait retransmit budget for response fragments (see
// EspNowPairing.h's responseSendAwaitingCb_ comment). Real-hardware
// testing saw ~1-2 radio-level send failures per 22-fragment response, so
// a handful of retries per fragment is ample; the whole drain still
// finishes far inside the Hub's 10s post-ack response deadline.
constexpr uint8_t kEspNowResponseFragMaxRetries = 5;
// Generous upper bound on how long a send callback may take before we
// assume it is never coming and retry the fragment anyway.
constexpr uint32_t kEspNowResponseFragCbTimeoutUs = 50000;

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
  arbiter_->send(AirtimeClass::Pairing, hubMac_, payload, sizeof(payload));
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
  serviceLinkStats();
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
  lastHubContactMs_ = millis();
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
  if (len == kEspNowTimeRespLen && data[0] == kEspNowTimeRespMagic) {
    const int64_t t4MonoUs = esp_timer_get_time();
    if (paired_ && timeHandler_ != nullptr) {
      timeHandler_(data, len, t4MonoUs, timeHandlerCtx_);
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
  arbiter_->send(AirtimeClass::Pairing, kEspNowBroadcastMac, payload, sizeof(payload));
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

  // Ack immediately, before even setting commandPending_, so the Hub's
  // EspNowCommandDispatcher can stop resending as soon as delivery is
  // confirmed rather than waiting for the full (possibly slow) response.
  // Sent unconditionally on every reassembled frame, including duplicates
  // from pre-ack resends still in flight -- idempotent and cheap.
  const uint8_t ackPayload[1] = {kEspNowControlAckMagic};
  arbiter_->send(AirtimeClass::Pairing, hubMac_, ackPayload, sizeof(ackPayload));

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
    // Stop-and-wait: one fragment is in flight at a time, and we only
    // advance once the radio confirms it was actually delivered. See
    // responseSendAwaitingCb_'s comment in the header for why the
    // synchronous esp_now_send() return value is not sufficient here.
    if (responseSendAwaitingCb_) {
      if (static_cast<int32_t>(nowUs - responseSendStartedUs_) <
          static_cast<int32_t>(kEspNowResponseFragCbTimeoutUs)) {
        return;  // still waiting for this fragment's verdict
      }
      // Callback never arrived -- treat as a failed send so we retry
      // rather than stalling the drain forever.
      responseSendAwaitingCb_ = false;
      responseSendCbFailed_ = true;
    }

    if (responseSendCbFailed_) {
      responseSendCbFailed_ = false;
      if (++responseFragRetries_ > kEspNowResponseFragMaxRetries) {
        Serial.printf("[espnow_pairing] response_frag_giving_up idx=%u/%u\n",
                      static_cast<unsigned>(responseFragsSent_),
                      static_cast<unsigned>(responseFragCount_));
        // Abandon the response: the Hub will report a delivery timeout,
        // which is at least an honest, bounded failure rather than a
        // drain that never finishes and blocks every later command.
        if (lifetime_ != nullptr) {
          lifetime_->noteEspNowReplyFailure();
        }
        responseFragCount_ = 0;
        responseFragsSent_ = 0;
        responseFragRetries_ = 0;
        return;
      }
      Serial.printf("[espnow_pairing] response_frag_retry idx=%u attempt=%u\n",
                    static_cast<unsigned>(responseFragsSent_),
                    static_cast<unsigned>(responseFragRetries_));
    }

    responseSendAwaitingCb_ = true;
    responseSendStartedUs_ = nowUs;
    if (arbiter_->send(AirtimeClass::CommandResponse, hubMac_,
                       responseFrags_[responseFragsSent_].bytes,
                       responseFrags_[responseFragsSent_].len) != ESP_OK) {
      // Rejected outright (e.g. ESP_ERR_ESPNOW_NO_MEM) -- no callback
      // will ever arrive for this one, so fail it immediately.
      responseSendAwaitingCb_ = false;
      responseSendCbFailed_ = true;
    }
    return;  // don't start processing a new command while still draining this response
  }

  if (pageCount_ > 0 && millis() - pagedTouchedMs_ >= kEspNowPagedResponseIdleMs) {
    Serial.printf("[espnow_pairing] paged_response_expired rid=%u\n",
                  static_cast<unsigned>(pagedResponseId_));
    releasePagedResponse();
  }

  if (!commandPending_ || control_ == nullptr) return;

  // A page request never reaches ControlServer: it is answered from the
  // cached reply, so the command that built it is not run again.
  // The token scan keeps ordinary (possibly 7 KB) commands from being
  // copied just to read their name.
  if (bufferContains(pendingCommandBuffer_, pendingCommandLen_, kPageRequestToken)) {
    const String request(reinterpret_cast<const char*>(pendingCommandBuffer_), pendingCommandLen_);
    if (jsonExtractString(request, "command", "") == "result_page") {
      commandPending_ = false;
      handlePageRequest(request);
      return;
    }
  }
  // Any other command means the Hub is done with the previous paged reply.
  releasePagedResponse();

  // Cleared after processCommand() returns, not before -- keeps
  // hasPendingCommandWork() true for the entire received-but-not-yet-
  // answered window, including this synchronous call itself, so
  // streamingGateOk() can hold off sensor streaming while it runs. Safe:
  // serviceEspNowCommand() copies out of pendingCommandBuffer_ at its own
  // top, and this is only ever called synchronously from loop().
  String response = control_->serviceEspNowCommand(pendingCommandBuffer_, pendingCommandLen_);
  commandPending_ = false;
  if (response.isEmpty()) {
    // Nothing to send back -- the Hub will sit on this command until its
    // own deadline expires and report command_delivery_timeout, which
    // gives no hint that the device actually processed it fine. Log it so
    // the two are distinguishable from the serial console.
    Serial.println("[espnow_pairing] empty_response nothing_to_send");
    return;
  }

  if (response.length() <= kEspNowMaxFrameBytes) {
    startResponse(reinterpret_cast<const uint8_t*>(response.c_str()), response.length());
    return;
  }

  // Too big for one frame: cache it and send page 0 as this command's
  // reply; the Hub pulls the rest with result_page (EspNowResultPager.h).
  const size_t bytes = response.length();
  pagedResponse_.reset(new (std::nothrow) String(std::move(response)));
  pageCount_ = pagedResponse_
                   ? EspNowResultPager::paginate(pagedResponse_->c_str(), pagedResponse_->length(),
                                                 kEspNowResultPageDataBudget, pageStarts_,
                                                 kEspNowResultMaxPages)
                   : 0;
  if (pageCount_ == 0) {
    // Only past kEspNowResultMaxPages (or out of heap): substitute a
    // compact error that always fits, so the caller gets a named failure
    // instead of an unexplained command_delivery_timeout.
    releasePagedResponse();
    Serial.printf("[espnow_pairing] response_too_large bytes=%u\n", static_cast<unsigned>(bytes));
    String fallback = "{\"ok\":false,\"error\":\"response_too_large\",";
    fallback += "\"message\":\"response_too_large\",\"data\":{\"response_bytes\":";
    fallback += static_cast<unsigned>(bytes);
    fallback += ",\"max_pages\":";
    fallback += static_cast<unsigned>(kEspNowResultMaxPages);
    fallback += "}}";
    startResponse(reinterpret_cast<const uint8_t*>(fallback.c_str()), fallback.length());
    return;
  }
  ++pagedResponseId_;
  pagedStartedMs_ = millis();
  Serial.printf("[espnow_pairing] paged_response rid=%u bytes=%u pages=%u\n",
                static_cast<unsigned>(pagedResponseId_), static_cast<unsigned>(bytes),
                static_cast<unsigned>(pageCount_));
  sendPage(0);
}

void EspNowPairing::handlePageRequest(const String& request) {
  long rid = -1;
  long chunk = -1;
  jsonExtractInt(request, "rid", rid);
  jsonExtractInt(request, "chunk", chunk);
  if (pageCount_ == 0 || rid != static_cast<long>(pagedResponseId_) || chunk < 0 ||
      static_cast<size_t>(chunk) >= pageCount_) {
    Serial.printf("[espnow_pairing] result_page_expired rid=%ld chunk=%ld cached_rid=%u pages=%u\n",
                  rid, chunk, static_cast<unsigned>(pagedResponseId_),
                  static_cast<unsigned>(pageCount_));
    static const char kExpired[] =
        "{\"ok\":false,\"cmd\":\"result_page\",\"error\":\"result_page_expired\","
        "\"message\":\"result_page_expired\"}";
    startResponse(reinterpret_cast<const uint8_t*>(kExpired), sizeof(kExpired) - 1);
    return;
  }
  sendPage(static_cast<size_t>(chunk));
}

void EspNowPairing::sendPage(size_t index) {
  const char* text = pagedResponse_->c_str();
  const size_t start = pageStarts_[index];
  const size_t end = index + 1 < pageCount_ ? pageStarts_[index + 1] : pagedResponse_->length();
  const size_t len =
      EspNowResultPager::pageLength(text, start, end, pagedResponseId_, index, pageCount_);
  std::unique_ptr<char[]> page(new (std::nothrow) char[len]);
  if (!page || EspNowResultPager::writePage(text, start, end, pagedResponseId_, index, pageCount_,
                                            page.get(), len) != len) {
    Serial.printf("[espnow_pairing] result_page_no_memory bytes=%u\n", static_cast<unsigned>(len));
    static const char kNoMemory[] =
        "{\"ok\":false,\"cmd\":\"result_page\",\"error\":\"result_page_no_memory\","
        "\"message\":\"result_page_no_memory\"}";
    startResponse(reinterpret_cast<const uint8_t*>(kNoMemory), sizeof(kNoMemory) - 1);
    return;
  }
  startResponse(reinterpret_cast<const uint8_t*>(page.get()), len);
  pagedTouchedMs_ = millis();
  if (index + 1 == pageCount_) {
    // Measured from page 0 leaving to the last page being queued: the
    // number to watch when tuning the paged transfer on real hardware.
    Serial.printf("[espnow_pairing] paged_response_last rid=%u pages=%u bytes=%u elapsed_ms=%u\n",
                  static_cast<unsigned>(pagedResponseId_), static_cast<unsigned>(pageCount_),
                  static_cast<unsigned>(pagedResponse_->length()),
                  static_cast<unsigned>(millis() - pagedStartedMs_));
  }
}

void EspNowPairing::releasePagedResponse() {
  pagedResponse_.reset();
  pageCount_ = 0;
}

void EspNowPairing::startResponse(const uint8_t* data, size_t len) {
  // frameId=0 is fine here since commands are processed synchronously and
  // one-at-a-time -- there's never a second response in flight to
  // disambiguate against.
  responseFragCount_ = EspNowFragmenter::fragment(data, len, 0, kEspNowFragTypeControl,
                                                  responseFrags_, kEspNowMaxFragCount);
  if (responseFragCount_ == 0) {
    // Callers only pass frames up to kEspNowMaxFrameBytes, so this is a
    // bug if it ever fires. It used to fall straight through, sending
    // nothing at all and silently: the device logged
    // `control_command_finished ok=true` while the Hub never saw a
    // fragment, which looked exactly like an RF failure.
    Serial.printf("[espnow_pairing] response_fragment_failed bytes=%u\n",
                  static_cast<unsigned>(len));
    static const char kFailed[] =
        "{\"ok\":false,\"error\":\"response_too_large\",\"message\":\"response_too_large\"}";
    responseFragCount_ = EspNowFragmenter::fragment(
        reinterpret_cast<const uint8_t*>(kFailed), sizeof(kFailed) - 1, 0, kEspNowFragTypeControl,
        responseFrags_, kEspNowMaxFragCount);
  }

  responseFragsSent_ = 0;
  responseFragRetries_ = 0;
  responseSendAwaitingCb_ = false;
  responseSendCbFailed_ = false;
}

void EspNowPairing::serviceLinkStats() {
  if (lifetime_ == nullptr) {
    return;
  }
  const uint32_t failures = pendingSendFailures_;
  if (failures != 0) {
    pendingSendFailures_ = pendingSendFailures_ - failures;
    lifetime_->noteEspNowSendFailures(failures);
  }
  if (!paired_) {
    return;
  }
  // A paired device hears its Hub several times a second (POLL) and at
  // least every HELLO round; this long without anything means the Hub went
  // away or out of range. Counted once per silence.
  const uint32_t now = millis();
  if (now - lastHubContactMs_ > kEspNowHubLostMs) {
    if (!hubLostCounted_) {
      hubLostCounted_ = true;
      lifetime_->noteEspNowHubLoss();
    }
  } else {
    hubLostCounted_ = false;
  }
}

void EspNowPairing::handleSendStatus(bool ok) {
  if (!ok) {
    // Plain read-modify-write: compound ops on volatile are deprecated, and
    // an occasional lost increment does not matter for a diagnostic count.
    pendingSendFailures_ = pendingSendFailures_ + 1;
  }
  if (!responseSendAwaitingCb_) return;  // not ours (ack / stream / OTA send)
  responseSendAwaitingCb_ = false;
  if (ok) {
    ++responseFragsSent_;
    responseFragRetries_ = 0;
  } else {
    responseSendCbFailed_ = true;
  }
}

bool EspNowPairing::consumePollPending() {
  if (!pollPending_) return false;
  pollPending_ = false;
  return true;
}

}  // namespace nhos
