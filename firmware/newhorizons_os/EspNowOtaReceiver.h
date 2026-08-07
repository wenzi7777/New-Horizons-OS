#pragma once

// Device-side counterpart to New-Horizons-Hub/newhorizons_hub/EspNowOtaRelay.h
// -- lets a device running in ESP-NOW/Direct mode (no WiFi/internet of its
// own) still receive OTA firmware updates, relayed by its paired Hub. See
// EspNowOtaRelay.h and ~/.claude/plans/linked-strolling-puppy.md's "New
// Horizons Direct 模式下的裝置 OTA" section for the full design.
//
// Two phases, both driven from a single boot-time trigger
// (EspNowPairing::onPaired() calls begin()+startBootCheck() once -- see
// EspNowPairing.cpp). Deliberately NOT a periodic recheck -- per explicit
// user direction, mirrors the WiFi path's own serviceAutoOta(), which also
// only runs once at boot:
//   1. fetch_manifest: ask the Hub to GET the manifest URL on this device's
//      behalf over kEspNowFragTypeHubRequest, since this device has no WiFi
//      of its own to do the GET itself. Parsed locally with this file's own
//      small manifest-field extraction -- NOT via OtaManager, which stays
//      HTTP-only/single-responsibility and is untouched by this feature.
//   2. ota_relay_start: if the manifest's version is newer, ask the Hub to
//      stream the firmware binary as a sequence of kEspNowFragTypeOta
//      chunks (stop-and-wait, each acked with a raw kOtaChunkAckMagic
//      packet -- mirrors EspNowOtaRelay.h's own resend model). Each chunk
//      is written incrementally via Update.write() + incremental SHA256,
//      the same pattern OtaManager::downloadAndApply() uses, just fed from
//      ESP-NOW chunks instead of an HTTPClient stream -- neither flash
//      partition table (device or Hub) has room to stage a full ~1.3-1.4MB
//      image, so this can never buffer-then-write.
//
// Idempotent by chunk_index: if the Hub resends a chunk because our ack was
// lost, this only re-sends the ack -- it does NOT call Update.write() a
// second time for an already-committed chunk (that would corrupt the flash
// image, since Update.write() always appends at the current offset).
//
// Known, deliberately accepted limitation (this is the direct consequence
// of "boot-time-once, no periodic recheck" above): if the Hub rejects the
// relay (relay_already_active -- another device's update is already in
// progress Hub-wide, see EspNowOtaRelay.h's single-session design) or the
// transfer itself fails/times out, this device does NOT retry until its
// next reboot.

#include <Arduino.h>

#include "mbedtls/sha256.h"

#include "DeviceConfig.h"
#include "OtaChunkCodec.h"

namespace nhos {

// MUST match New-Horizons-Hub/newhorizons_hub/EspNowHubManager.h's
// kOtaChunkAckMagic/kOtaChunkAckLen exactly -- duplicated, not shared (see
// EspNowPairing.h's magic-byte comment for why: separate Arduino sketch
// trees, no symlinks).
constexpr uint8_t kOtaChunkAckMagic = 0xE6;
constexpr size_t kOtaChunkAckLen = 3;

// kOtaChunkSubHeaderLen itself lives in OtaChunkCodec.h (shared with its
// host-native unit tests). MUST match EspNowOtaRelay.h's own
// kOtaChunkSubHeaderLen/kOtaChunkPayloadBytes.
constexpr size_t kOtaChunkPayloadBytes =
    16 /* kEspNowMaxFragCount */ * 240 /* kEspNowFragMaxPayload */ - kOtaChunkSubHeaderLen;

// Retry model for the two small hub-request round trips (fetch_manifest /
// ota_relay_start) -- much shorter/fewer than EspNowOtaRelay.h's own
// per-chunk resend budget (0.5s*30=15s) since these are one-shot requests
// where giving up quickly and waiting for the next reboot (see file header)
// is an acceptable, deliberately simple failure mode.
constexpr uint32_t kHubRequestResendIntervalMs = 800;
constexpr uint8_t kHubRequestMaxAttempts = 5;

class EspNowOtaReceiver {
 public:
  // Called once from EspNowPairing::onPaired(). hubMac is copied, not
  // referenced -- safe even though the caller's own hubMac_ buffer outlives
  // this anyway.
  void begin(DeviceConfig& deviceConfig, const uint8_t hubMac[6]);

  // Called once from EspNowPairing::onPaired(), right after begin(). No-op
  // if already called this boot (bootCheckStarted_ guard) -- see file
  // header for why this is deliberately not re-armed periodically.
  void startBootCheck();

  // On-demand entry points for ControlServer's `check_update` /
  // `apply_update` commands while this device is in ESP-NOW/Direct mode.
  //
  // Those two commands normally drive OtaManager over HTTP, which cannot
  // work here: a Direct-mode device never joins WiFi, so the manifest GET
  // fails outright (observed as `manifest_http_-1` from the WebUI). They
  // now route here instead, so the Hub-relayed path that already works at
  // boot is also reachable on demand.
  //
  // Unlike startBootCheck() these are re-armable -- the boot-once rule in
  // the file header is about *automatic* rechecks, not about refusing an
  // explicit user request. Both return false if a check/transfer is
  // already running (or the device isn't paired yet), so a second click
  // can't corrupt an in-flight update.
  //
  // Both are asynchronous: the manifest lives on the Hub's side of a
  // request/reply round trip, so the result appears in statusJson() a
  // moment later rather than in the command's own reply. That matches how
  // apply_update already behaves on the HTTP path (returns
  // "update_started", caller polls update_state).
  bool requestCheck(const String& manifestUrl);
  bool requestApply(const String& manifestUrl);

  // Same field shape as OtaManager::lastStatusJson() so the Desktop app's
  // existing update_state rendering (phase/version/url/last_error) works
  // unchanged for Direct-mode devices. ControlServer substitutes this for
  // the OtaManager one in `status` when a receiver is attached.
  String statusJson() const;

  // Call every loop() iteration (from newhorizons_os.ino, parallel to
  // espNowPairing.service() -- mirrors newhorizons_hub.ino's own
  // otaRelay.service() call alongside hubManager.service()).
  void service();

  // Wired from EspNowPairing::service()'s hub-request-frame dispatch (a
  // completed kEspNowFragTypeHubRequest frame -- either a fetch_manifest or
  // ota_relay_start reply).
  void handleHubRequestFrame(const uint8_t* data, size_t len);

  // Wired from EspNowPairing::service()'s OTA-frame dispatch (a completed
  // kEspNowFragTypeOta frame -- one firmware chunk, sub-header + payload).
  void handleOtaChunkFrame(const uint8_t* data, size_t len);

  // True only during the actual chunk-by-chunk transfer (not the brief
  // manifest-check/relay-negotiation phases beforehand) -- checked by
  // newhorizons_os.ino's streamingGateOk() to pause this device's own
  // sensor-data uploads while a relay is in progress. Real-hardware
  // testing found the two compete for the same ESP-NOW airtime, making
  // the relay far slower than expected when sensor streaming continued
  // unpaused alongside it.
  bool isRelaying() const { return phase_ == Phase::kRelaying; }

 private:
  enum class Phase : uint8_t {
    kIdle,
    kAwaitingManifestReply,
    kAwaitingRelayStartReply,
    kRelaying,
    kFinished,
  };

  bool startOnDemand(const String& manifestUrl, bool apply);
  const char* phaseName() const;
  void sendHubRequest(const String& json);
  void sendManifestRequest();
  void sendRelayStartRequest();
  void sendChunkAck(uint16_t chunkIndex);
  void handleManifestReply(const String& payload);
  void handleRelayStartReply(const String& payload);
  void finishRelay();
  void abortRelay(const char* reason);
  int compareVersion(const String& remote, const String& local) const;

  DeviceConfig* deviceConfig_ = nullptr;
  uint8_t hubMac_[6] = {0};
  String manifestUrl_;

  bool bootCheckStarted_ = false;
  bool bootCheckPending_ = false;
  Phase phase_ = Phase::kIdle;
  uint32_t requestSentMs_ = 0;
  uint8_t requestAttempts_ = 0;

  String pendingVersion_;
  String pendingUrl_;
  String pendingSha256_;
  String pendingChangelogUrl_;
  size_t pendingSize_ = 0;

  // Reported through statusJson(). applyRequested_ is what separates a
  // check ("is there a newer build?" -- stop after comparing versions)
  // from an apply ("go get it" -- continue into the relay). The boot-time
  // path sets it true, matching the WiFi path's own autoApplyOnBoot
  // behaviour.
  bool applyRequested_ = false;
  bool updateAvailable_ = false;
  String operation_;
  String lastError_;
  String lastResult_;

  uint16_t totalChunks_ = 0;
  uint16_t nextExpectedChunk_ = 0;
  bool hasWrittenAnyChunk_ = false;
  mbedtls_sha256_context shaCtx_;
};

}  // namespace nhos
