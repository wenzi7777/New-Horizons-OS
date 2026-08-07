#pragma once

// Device-side ESP-NOW pairing + command handling. Counterpart to
// firmware/newhorizons_hub/EspNowHubManager.h -- see that file (and
// ~/.claude/plans/linked-strolling-puppy.md) for the full HELLO/PAIRED/POLL
// protocol design and why it's event-driven rather than a fixed schedule.
//
// Normally this device targets a specific, already-known Hub MAC (set via
// DeviceConfig.transport.hubMac, e.g. by the `set_transport` command from
// New Horizons Direct's "convert to local pairing" flow). What it
// *doesn't* know in that case is which 2.4GHz channel that Hub is
// currently on, since the Hub's channel follows whatever its own WiFi STA
// uplink is associated on (see EspNowHubManager.h) -- so this scans a
// small set of candidate channels while unpaired.
//
// If hub_mac is empty/invalid at begin() (e.g. a factory-fresh device
// that has never been reachable via any Gateway to receive a
// set_transport command), this instead broadcasts the same HELLO byte to
// FF:FF:FF:FF:FF:FF while channel-hopping -- see serviceDiscovery(). Any
// Hub in range replies exactly as it would to a unicast HELLO (Hub-side
// code is unchanged and unaware of the distinction -- it only ever looks
// at the sender's real MAC, never the frame's destination address). Once
// a reply is seen, the discovered MAC is saved and the device reboots
// into the normal, unmodified unicast flow above.

#include <Arduino.h>

#include "Config.h"
#include "DeviceConfig.h"
#include "EspNowFrame.h"
#include "Storage.h"

namespace nhos {

class ControlServer;
class EspNowOtaReceiver;

// Scratch size for the device-initiated hub-request reassembler
// (fetch_manifest/ota_relay_start replies) -- MUST match
// New-Horizons-Hub/newhorizons_hub/EspNowHubManager.h's own
// kHubRequestBufferBytes. Deliberately much smaller than the OTA/control
// reassemblers' kEspNowMaxFragCount*kEspNowFragMaxPayload (3840B), since
// these payloads are small JSON asks/replies, not firmware chunks.
constexpr size_t kEspNowHubRequestScratchBytes = 1024;

// MUST match firmware/newhorizons_hub/EspNowHubManager.h's
// kHubHelloMagic/kHubPairedMagic/kHubPollMagic exactly -- duplicated, not
// shared, since the Hub and device are separate Arduino sketch trees (see
// newhorizons_hub/README.md's "shared files" note for the general pattern
// this project uses instead of symlinks).
constexpr uint8_t kEspNowHelloMagic = 0xE1;
constexpr uint8_t kEspNowPollMagic = 0xE2;
constexpr uint8_t kEspNowPairedMagic = 0xE3;

// Sent back to the Hub the instant a control-command frame finishes
// reassembling, before commandPending_ is even set -- lets
// EspNowCommandDispatcher stop blind-resending the raw command as soon as
// it's confirmed received, rather than only once the (possibly slow,
// possibly multi-fragment) response fully arrives. Mirrors kOtaChunkAckMagic
// (EspNowOtaReceiver.h) in spirit but carries no payload -- only one
// command is ever in flight per device, so there's nothing to disambiguate.
// MUST match New-Horizons-Hub/newhorizons_hub/EspNowHubManager.h's own
// kEspNowControlAckMagic exactly (duplicated, not shared -- see this file's
// header comment on kEspNowHelloMagic).
constexpr uint8_t kEspNowControlAckMagic = 0xE7;

// All valid 2.4GHz channels (1-13, the Japan/EU/most-regions range --
// channels 12-13 are unused in the US but harmlessly scanned there too).
// Real-hardware testing found a university AP that auto-selected channel
// 9 -- assuming APs only use the "standard" non-overlapping 1/6/11 trio
// (an earlier version of this scan list) missed it entirely, so this
// scans everything. DeviceConfig.transport.lastKnownChannel (if non-zero)
// is tried first, ahead of this list, so a previously-paired device
// doesn't pay the full scan cost on every reconnect.
constexpr uint8_t kEspNowScanChannels[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
constexpr uint8_t kEspNowMaxScanChannels = 14;  // lastKnownChannel + the 13 above
constexpr uint32_t kEspNowChannelDwellMs = 400;
constexpr uint32_t kEspNowHelloRetryMs = 1500;

constexpr uint8_t kEspNowBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Shared "give up and fall back to the WiFi setup portal" timeout, applied
// from begin() regardless of whether hub_mac was empty (discovering_) or a
// known-but-currently-unreachable MAC -- both cases are the same "hasn't
// paired since this boot's begin()" watchdog, just with different entry
// conditions (empty hub_mac vs. a Hub that's off/moved/factory-reset).
// 120s is a placeholder pending real-hardware measurement of worst-case
// channel-hop find time (see ~/.claude/plans/linked-strolling-puppy.md's
// "新裝置免 Gateway 直接配對 Hub" section) -- tune from that, don't leave
// it as a guess once real numbers exist. Comfortably longer than a Hub's
// own OTA-reboot window (10-30s), so a Hub briefly rebooting doesn't
// bounce a paired device back to the portal.
constexpr uint32_t kEspNowPairingTimeoutMs = 120000;
// NVS key (Preferences 15-char limit -- this is 14). Value unchanged from
// this constant's earlier discovery-only name/scope -- no NVS migration
// needed, this is a rename for code clarity only.
constexpr char kEspNowPairingFailFlagKey[] = "espnow_disc_fb";

class EspNowPairing {
 public:
  EspNowPairing();

  bool begin(Storage& storage, DeviceConfig& deviceConfig, ControlServer& control,
             const uint8_t uid[6]);

  // Call every loop() iteration.
  void service();

  // Wired to the global esp_now_recv callback in newhorizons_os.ino.
  void handleEspNowRecv(const uint8_t mac[6], const uint8_t* data, size_t len);

  bool hasHub() const { return paired_; }
  const uint8_t* hubMac() const { return hubMac_; }

  // True from the moment a control-command frame finishes reassembling
  // (commandPending_) through processCommand() execution and the paced
  // response-fragment drain (responseFragsSent_ < responseFragCount_).
  // Wired into newhorizons_os.ino's streamingGateOk() to pause this
  // device's own sensor-data uploads for the same airtime-contention
  // reason already validated for EspNowOtaReceiver::isRelaying() -- see
  // that getter's comment.
  bool hasPendingCommandWork() const { return commandPending_ || responseFragsSent_ < responseFragCount_; }

  // EspNowStreamTransport calls this once per loop(); returns true (and
  // clears the flag) at most once per POLL received.
  bool consumePollPending();

  // Wired once from newhorizons_os.ino's setup(), before the recv callback
  // is registered -- see EspNowOtaReceiver.h. onPaired() hands off the
  // boot-time OTA manifest check to it; service() dispatches completed
  // kEspNowFragTypeOta/kEspNowFragTypeHubRequest frames to it. Optional --
  // left null, this device simply never attempts Direct-mode OTA.
  void setOtaReceiver(EspNowOtaReceiver* receiver) { otaReceiver_ = receiver; }

 private:
  void buildScanChannelList();
  void startScanIfNeeded();
  void sendHello();
  void onPaired();
  void handleFragmentedPacket(const uint8_t* data, size_t len);
  void serviceCommand();
  bool parseHubMacFromConfig();
  void serviceDiscovery();
  void sendDiscoveryHello();
  void onHubDiscovered(const uint8_t mac[6]);
  void failPairingAndRestart(const char* reason);

  Storage* storage_ = nullptr;
  DeviceConfig* deviceConfig_ = nullptr;
  ControlServer* control_ = nullptr;
  uint8_t uid_[6] = {0};

  uint8_t hubMac_[6] = {0};
  bool hubMacValid_ = false;
  bool paired_ = false;

  // True from begin() (when hub_mac was empty/invalid) until a Hub
  // replies or discovery times out. Mutually exclusive with the normal
  // hubMacValid_-gated flow below by construction -- see service() and
  // handleEspNowRecv().
  bool discovering_ = false;

  // Stamped unconditionally in begin() (regardless of discovering_), so
  // the shared kEspNowPairingTimeoutMs watchdog in service() covers both
  // the empty-hub_mac discovery path and the known-but-unreachable-hub_mac
  // path with one mechanism -- see failPairingAndRestart().
  uint32_t pairingAttemptStartMs_ = 0;

  uint8_t scanChannels_[kEspNowMaxScanChannels] = {0};
  uint8_t scanChannelCount_ = 0;
  uint8_t scanIndex_ = 0;
  uint32_t lastChannelSwitchMs_ = 0;
  uint32_t lastHelloMs_ = 0;

  volatile bool pollPending_ = false;

  uint8_t controlScratch_[kEspNowMaxFragCount * kEspNowFragMaxPayload];
  EspNowReassembler controlReassembler_;

  // Separate reassembler + buffer for kEspNowFragTypeOta frames (Hub-pushed
  // firmware chunks) -- mirrors EspNowHubManager.h's DeviceSlot rationale
  // for never sharing a reassembler across frame types (a new frame's
  // first fragment abandons whatever's in flight, which would otherwise
  // corrupt an in-progress OTA chunk if it interleaved with a control
  // command). Copied out of the reassembler's own scratch (only valid
  // until the next onFragment() call) into otaFrameBuffer_ so service()
  // can process it after the recv callback returns -- same
  // buffer-in-callback/process-in-loop split as pendingCommandBuffer_
  // below (Update.write() + SHA256 are far too much work for the small
  // ESP-NOW/WiFi driver task stack).
  uint8_t otaScratch_[kEspNowMaxFragCount * kEspNowFragMaxPayload];
  EspNowReassembler otaReassembler_;
  uint8_t otaFrameBuffer_[kEspNowMaxFragCount * kEspNowFragMaxPayload];
  size_t otaFrameBufferLen_ = 0;
  volatile bool otaFrameReady_ = false;

  // Same pattern again, for device-initiated kEspNowFragTypeHubRequest
  // replies (fetch_manifest/ota_relay_start). Smaller buffer -- see
  // kEspNowHubRequestScratchBytes.
  uint8_t hubRequestScratch_[kEspNowHubRequestScratchBytes];
  EspNowReassembler hubRequestReassembler_;
  uint8_t hubRequestFrameBuffer_[kEspNowHubRequestScratchBytes];
  size_t hubRequestFrameBufferLen_ = 0;
  volatile bool hubRequestFrameReady_ = false;

  EspNowOtaReceiver* otaReceiver_ = nullptr;

  // A fully-reassembled control command is copied here and only handed to
  // ControlServer::serviceEspNowCommand() from service() (main-loop
  // context). Found on real hardware: calling serviceEspNowCommand()
  // directly from handleControlFragment() -- which runs on the ESP-NOW/
  // WiFi driver task, not the main Arduino task -- overflowed that task's
  // (much smaller) stack while building a large response like "status",
  // crashing with a stack-canary panic. Mirrors the buffer-in-callback/
  // process-in-loop split firmware/newhorizons_hub/EspNowHubManager.h
  // already uses for the same reason.
  uint8_t pendingCommandBuffer_[kEspNowMaxFragCount * kEspNowFragMaxPayload];
  size_t pendingCommandLen_ = 0;
  volatile bool commandPending_ = false;

  // Response-side fragment buffer for serviceCommand(). A class member
  // rather than a function-local array on purpose: found on real
  // hardware that a ~4KB local EspNowFragment[kEspNowMaxFragCount] array,
  // reserved in serviceCommand()'s stack frame for the whole duration of
  // the (much deeper, String-heavy) processCommand() call it wraps, was
  // enough extra stack pressure to overflow the main loop task's stack
  // while building a large response like "status" -- manifesting as heap
  // corruption inside String::reserve()'s realloc() rather than tripping
  // the stack canary (which is why it looked like a different, unrelated
  // crash at first). Static storage removes that stack cost entirely.
  //
  // Sending is paced across service() calls (same reason
  // EspNowStreamTransport.cpp paces its own fragment bursts): found on
  // real hardware that firing all of a multi-fragment response's
  // esp_now_send() calls back-to-back in one go gets most of them
  // silently dropped (ESP_ERR_ESPNOW_NO_MEM) for anything bigger than a
  // couple of fragments, e.g. the ~11-fragment "status" response -- the
  // Hub never saw a single fragment from it. A single-fragment response
  // (e.g. "reboot"'s tiny ack) happens to send within one service() call
  // regardless, which is why this wasn't caught until "status" was tested.
  EspNowFragment responseFrags_[kEspNowMaxFragCount];
  uint8_t responseFragCount_ = 0;
  uint8_t responseFragsSent_ = 0;
  uint32_t responseFragIntervalUs_ = 0;
  uint32_t responseNextFragDueUs_ = 0;
};

}  // namespace nhos
