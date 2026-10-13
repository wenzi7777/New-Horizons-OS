#pragma once

#include <Arduino.h>

#include "ClockCore.h"
#include "ClockSources.h"

namespace nhos {

class DeviceConfig;

// The device clock: one read API for everything that needs wall time, with
// the source picked internally by transport mode.
//
//   WiFi    -> the Gateway over UDP   (four-timestamp exchange; preferred)
//              then NTP               (SntpClockSource; when no Gateway answers)
//   ESP-NOW -> the Hub over ESP-NOW   (the same exchange)
//   either  -> `set_time`             (manual / Backend push; lowest priority)
//
// Wall time is ClockCore's model over esp_timer's monotonic microseconds,
// not the libc system clock: corrections are slewed so packet timestamps
// never run backwards, and a sample can be stamped at the instant it was
// taken (epochMsAt) rather than whenever the packet happened to be built.
// The system clock is still stepped alongside, so time() stays sensible.
//
// Threading: ClockCore is only touched from the main loop. Sources that are
// called back from other tasks (SNTP from tcpip, ESP-NOW from the WiFi task)
// park their sample in a lock-protected mailbox drained by service().
class ClockService {
 public:
  void configure(const DeviceConfig& config);
  // Transport is fixed at boot, so so is the clock's source.
  void setEspNowMode(bool espNow);
  // WiFi mode, once connected; idempotent, safe to call every loop pass.
  void beginSntp();
  // ESP-NOW mode.
  void attachHub(EspNowPairing* pairing, AirtimeArbiter* arbiter);
  // WiFi mode: the Gateway is reached the same way sensor frames are.
  void attachGateway(UdpStreamTransport* transport);
  // TIME_RESP receive paths; `t4MonoUs` stamped as soon as it was seen.
  void onHubTimeResponse(const uint8_t* data, size_t len, int64_t t4MonoUs) {
    hub_.onResponse(data, len, t4MonoUs);
  }
  void onGatewayTimeResponse(const uint8_t* data, size_t len, int64_t t4MonoUs) {
    gateway_.onResponse(data, len, t4MonoUs);
  }
  void service(uint32_t nowMs);
  // `clock_sync`: re-sync from whichever source this mode uses, now.
  // Returns false when there is no source to ask (e.g. WiFi not connected).
  // `reprobe` (user-initiated) also re-asks a server judged too old.
  bool requestSync(bool reprobe = true);

  bool hasSynced() const { return core_.synced(); }
  uint64_t nowEpochMs() const;
  // Wall time at a past esp_timer_get_time() reading; 0 when not synced.
  uint64_t epochMsAt(int64_t monoUs) const;
  uint32_t uptimeMs() const { return millis(); }
  TimeSource source() const { return core_.source(); }
  uint32_t uncertaintyMs() const;
  int64_t lastAdjustMs() const { return core_.lastAdjustUs() / 1000; }

  // `set_time`. Rejects pre-2020 values so a zero/garbage push cannot
  // destroy a good clock; `force` (an explicit user action) also overrides a
  // better source that is currently in charge.
  ClockSubmitResult setEpochMs(uint64_t epochMs, TimeSource source, bool force = false);
  // Used by the sources, from the main loop only.
  ClockSubmitResult submit(const ClockSample& sample, bool force = false);

  // False once the boot grace has passed with a source that should have
  // produced time but has not. A Hub without the time service is not counted
  // (see the .cpp); it shows as status.clock.hub.supported=false instead.
  bool healthy() const;
  String statusJson() const;

  static int64_t monoUs();

 private:
  ClockCore core_;
  SntpClockSource sntp_;
  EspNowTimeLink hubLink_;
  UdpTimeLink gatewayLink_;
  TimeExchangeSource hub_{TimeSource::Hub};
  TimeExchangeSource gateway_{TimeSource::Gateway};
  bool espNowMode_ = false;
  uint32_t lastResultMs_ = 0;
  ClockSubmitResult lastResult_ = ClockSubmitResult::Rejected;
};

}  // namespace nhos
