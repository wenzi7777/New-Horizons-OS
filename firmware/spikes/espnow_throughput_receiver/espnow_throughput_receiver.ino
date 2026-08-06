// Phase 0 spike receiver -- counterpart to espnow_throughput_sender.ino.
// See plan section 6.1. Flash this to one board first, note the printed
// MAC address, then flash espnow_throughput_sender to 1-4 other boards
// with that MAC filled into kReceiverMac.
//
// Tracks up to kMaxPeers concurrent senders by source MAC, reassembles
// fragments back into full frames, and prints per-peer throughput/loss
// stats every 5s -- this is the number the plan's Phase 0 exit criteria
// depends on before any Hub/device product firmware gets written.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <cstring>

#include "EspNowFrame.h"

namespace {

constexpr uint8_t kEspNowChannel = 6;  // must match the senders
constexpr uint8_t kMaxPeers = 4;       // matches the plan's 2-4 device target
constexpr size_t kScratchBytes =
    nhos::kEspNowMaxFragCount * nhos::kEspNowFragMaxPayload;

// Hub-driven unicast polling (plan section 6a, 2nd iteration): a broadcast
// sync beacon helped but did not fully fix 4-concurrent-device loss
// (measured: 1 of 4 peers recovered, the other 3 still showed 36-90%
// loss) -- broadcast 802.11 frames get no MAC-layer ACK/retry, so some
// devices were plausibly missing beacons outright with no way to notice or
// recover. Replaced with the Hub explicitly polling one device at a time
// over *unicast* (which does get MAC-layer ACK+retry, i.e. actually
// reliable delivery): each registered device only transmits when it
// receives a POLL addressed to it, and the Hub gives it a fixed time
// budget before moving on to the next registered device, round-robin.
constexpr uint8_t kHelloMagic = 0xE1;  // device -> Hub, "I exist, register me"
constexpr uint8_t kPollMagic = 0xE2;   // Hub -> device, "your turn now"
constexpr uint16_t kTargetFps = 24;    // must match sender kTargetFps
constexpr uint32_t kFrameIntervalUs = 1000000UL / kTargetFps;
// Fixed per-device budget assuming up to kMaxPeers devices; if fewer are
// actually registered, the round-robin cycles through just those, so they
// naturally get polled more often (higher effective fps) -- not a bug.
constexpr uint32_t kPollBudgetUs = kFrameIntervalUs / kMaxPeers;
uint32_t nextPollDueUs = 0;
uint8_t pollCursor = 0;

class PeerSlot {
 public:
  PeerSlot() : reassembler(scratch, sizeof(scratch)) {}

  bool used = false;
  bool registered = false;
  uint8_t mac[6] = {0};
  uint8_t scratch[kScratchBytes];
  nhos::EspNowReassembler reassembler;

  uint32_t fragmentsSeen = 0;
  uint32_t framesComplete = 0;
  uint32_t bytesComplete = 0;
  bool haveFrameId = false;
  uint16_t firstFrameId = 0;
  uint16_t lastFrameId = 0;
};

PeerSlot slots[kMaxPeers];
uint32_t lastReportMs = 0;

int findOrCreateSlot(const uint8_t* mac) {
  for (uint8_t i = 0; i < kMaxPeers; ++i) {
    if (slots[i].used && memcmp(slots[i].mac, mac, 6) == 0) {
      return i;
    }
  }
  for (uint8_t i = 0; i < kMaxPeers; ++i) {
    if (!slots[i].used) {
      slots[i].used = true;
      memcpy(slots[i].mac, mac, 6);
      return i;
    }
  }
  return -1;  // more senders on the air than kMaxPeers -- spike over capacity
}

void registerPeerIfNeeded(PeerSlot& slot) {
  if (slot.registered) return;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, slot.mac, 6);
  peer.channel = kEspNowChannel;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) == ESP_OK) {
    slot.registered = true;
    Serial.printf("[receiver] registered peer %02x:%02x:%02x:%02x:%02x:%02x\n",
                  slot.mac[0], slot.mac[1], slot.mac[2], slot.mac[3],
                  slot.mac[4], slot.mac[5]);
  }
}

void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const int idx = findOrCreateSlot(info->src_addr);
  if (idx < 0) return;
  PeerSlot& slot = slots[idx];

  if (len == 1 && data[0] == kHelloMagic) {
    registerPeerIfNeeded(slot);
    return;
  }

  slot.fragmentsSeen++;

  nhos::ReassembledFrame frame;
  if (slot.reassembler.onFragment(data, static_cast<size_t>(len), &frame)) {
    slot.framesComplete++;
    slot.bytesComplete += static_cast<uint32_t>(frame.len);
    if (!slot.haveFrameId) {
      slot.firstFrameId = frame.frameId;
      slot.haveFrameId = true;
    }
    slot.lastFrameId = frame.frameId;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(kEspNowChannel, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[receiver] esp_now_init FAILED");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);

  // WiFi.macAddress() was observed returning 00:00:00:00:00:00 on this
  // core/board even after esp_now_init() -- read straight from the
  // ESP-IDF API instead of the Arduino wrapper.
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  Serial.printf("[receiver] my_mac=%02X:%02X:%02X:%02X:%02X:%02X\n", mac[0],
                mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println(
      "[receiver] copy the MAC above into kReceiverMac in "
      "espnow_throughput_sender.ino");

  Serial.printf("[receiver] ready: up to %u concurrent senders, channel=%u\n",
                kMaxPeers, kEspNowChannel);
}

void loop() {
  const uint32_t nowUs = micros();
  if (static_cast<int32_t>(nowUs - nextPollDueUs) >= 0) {
    nextPollDueUs += kPollBudgetUs;
    for (uint8_t tries = 0; tries < kMaxPeers; ++tries) {
      const uint8_t idx = pollCursor;
      pollCursor = (pollCursor + 1) % kMaxPeers;
      if (slots[idx].used && slots[idx].registered) {
        const uint8_t payload[1] = {kPollMagic};
        esp_now_send(slots[idx].mac, payload, sizeof(payload));
        break;
      }
    }
  }

  const uint32_t now = millis();
  if (now - lastReportMs < 5000) {
    return;
  }
  lastReportMs = now;

  for (uint8_t i = 0; i < kMaxPeers; ++i) {
    PeerSlot& slot = slots[i];
    if (!slot.used) continue;

    // Loss estimate: gaps in the sender's monotonically-increasing frameId
    // over this 5s report window (frameId wraps at 16 bits, ignored here --
    // a spike run is expected to last well under 65536 frames / fps).
    uint32_t expectedFrames = 0;
    if (slot.haveFrameId) {
      expectedFrames = static_cast<uint32_t>(static_cast<uint16_t>(
                            slot.lastFrameId - slot.firstFrameId)) +
                        1;
    }
    const float lossPct =
        expectedFrames > 0
            ? 100.0f * (1.0f - static_cast<float>(slot.framesComplete) /
                                    static_cast<float>(expectedFrames))
            : 0.0f;
    const float approxFps = slot.framesComplete / 5.0f;
    const float approxKbps = (slot.bytesComplete * 8.0f / 1000.0f) / 5.0f;

    Serial.printf(
        "[peer %02x:%02x:%02x:%02x:%02x:%02x] frames_ok=%lu frags_seen=%lu "
        "approx_fps=%.1f approx_kbps=%.1f loss=%.1f%%\n",
        slot.mac[0], slot.mac[1], slot.mac[2], slot.mac[3], slot.mac[4],
        slot.mac[5], static_cast<unsigned long>(slot.framesComplete),
        static_cast<unsigned long>(slot.fragmentsSeen), approxFps, approxKbps,
        lossPct);

    slot.framesComplete = 0;
    slot.fragmentsSeen = 0;
    slot.bytesComplete = 0;
    slot.haveFrameId = false;
  }
}
