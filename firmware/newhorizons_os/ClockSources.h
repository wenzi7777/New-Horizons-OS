#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include "ClockCore.h"
#include "Config.h"
#include "EspNowTimeWire.h"

namespace nhos {

class AirtimeArbiter;
class ClockService;
class EspNowPairing;
class UdpStreamTransport;

// NTP via ESP-IDF's SNTP client. The library's weak sntp_sync_time() hook
// is replaced (ClockSources.cpp) so a new NTP reading becomes a ClockCore
// sample instead of an unconditional settimeofday() step.
class SntpClockSource {
 public:
  void configure(const String& server, uint32_t intervalS);
  // Idempotent. Applies the configured server/interval on first call.
  void begin();
  void restart();
  bool started() const { return started_; }
  // tcpip task: park the reading for drain().
  void onSntpTime(int64_t epochUs, int64_t monoUs);
  void drain(ClockService& clock);
  String statusJson() const;

 private:
  bool started_ = false;
  // esp_sntp_setservername() keeps the pointer, not a copy.
  char server_[kMaxNtpServerLen + 1] = {0};
  uint32_t intervalS_ = kDefaultNtpIntervalS;
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  volatile bool pending_ = false;
  ClockSample pendingSample_;
  uint32_t syncCount_ = 0;
  uint32_t lastSyncMs_ = 0;
};

// How a TimeExchangeSource reaches its time server. The exchange itself
// (EspNowTimeWire's four-timestamp TIME_REQ/TIME_RESP) is the same whether
// it rides ESP-NOW to a Hub or UDP to a Gateway; only delivery differs.
class TimeExchangeLink {
 public:
  virtual ~TimeExchangeLink() = default;
  // There is a server to ask (paired with a Hub / a Gateway is known).
  virtual bool linkUp() const = 0;
  // Radio is free for us right now; false defers the request a pass.
  virtual bool canSendNow() const { return true; }
  virtual bool send(const uint8_t* data, size_t len) = 0;
};

// ESP-NOW to the paired Hub, through the airtime arbiter.
class EspNowTimeLink : public TimeExchangeLink {
 public:
  void attach(EspNowPairing* pairing, AirtimeArbiter* arbiter) {
    pairing_ = pairing;
    arbiter_ = arbiter;
  }
  bool linkUp() const override;
  bool canSendNow() const override;
  bool send(const uint8_t* data, size_t len) override;

 private:
  EspNowPairing* pairing_ = nullptr;
  AirtimeArbiter* arbiter_ = nullptr;
};

// UDP to the Gateway, on the same socket and destination as sensor frames
// (so it reaches whichever Gateway FindMe attached to).
class UdpTimeLink : public TimeExchangeLink {
 public:
  void attach(UdpStreamTransport* transport) { transport_ = transport; }
  bool linkUp() const override;
  bool send(const uint8_t* data, size_t len) override;

 private:
  UdpStreamTransport* transport_ = nullptr;
};

// The device half of the time exchange (EspNowTimeWire.h), against a Hub or
// a Gateway. Requests periodically, keeps the lowest-delay round trips in a
// ClockFilter, and backs off if the server turns out not to answer at all.
class TimeExchangeSource {
 public:
  // Back-to-back requests on (re)start, so the filter has a choice of
  // round trips before the first sync rather than trusting just one.
  static constexpr uint8_t kBurstCount = 4;
  static constexpr uint32_t kBurstSpacingMs = 250;
  static constexpr uint32_t kResponseTimeoutMs = 200;
  // A round trip longer than this was queued somewhere; its asymmetry could
  // be most of that, so it is not worth keeping.
  static constexpr uint32_t kMaxDelayUs = 20000;
  // Unanswered requests in a row before deciding the server predates the
  // time service, and how often to look again after that.
  static constexpr uint8_t kUnsupportedAfterMisses = 5;
  static constexpr uint32_t kUnsupportedRetryMs = 5UL * 60UL * 1000UL;

  explicit TimeExchangeSource(TimeSource source) : source_(source) {}

  void attach(TimeExchangeLink* link) { link_ = link; }
  bool attached() const { return link_ != nullptr; }
  void setIntervalS(uint16_t intervalS) { intervalMs_ = static_cast<uint32_t>(intervalS) * 1000UL; }
  // `reprobe` also forgets a "server too old" verdict -- for an explicit
  // user sync, not for automatic retries (which would defeat the back-off).
  void burst(bool reprobe = true);
  void service(uint32_t nowMs, ClockService& clock);
  // Receive path (ESP-NOW callback on the WiFi task, or the UDP poll);
  // `t4MonoUs` is stamped as soon as the reply is seen.
  void onResponse(const uint8_t* data, size_t len, int64_t t4MonoUs);

  bool linkUp() const { return link_ != nullptr && link_->linkUp(); }
  bool supported() const { return !unsupported_; }
  String statusJson() const;

 private:
  void sendRequest(uint32_t nowMs);
  void handleResponse(ClockService& clock);

  const TimeSource source_;
  TimeExchangeLink* link_ = nullptr;
  uint32_t intervalMs_ = static_cast<uint32_t>(kDefaultSyncIntervalS) * 1000UL;
  bool wasUp_ = false;
  uint8_t burstRemaining_ = 0;
  uint32_t nextRequestMs_ = 0;

  uint8_t seq_ = 0;
  bool awaiting_ = false;
  int64_t requestT1Us_ = 0;
  uint32_t requestSentMs_ = 0;
  uint8_t misses_ = 0;
  bool unsupported_ = false;
  // Once this server has answered, silence means busy/lost, not unsupported.
  bool answered_ = false;

  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  volatile bool mailboxFull_ = false;
  uint8_t mailbox_[kEspNowTimeRespLen] = {0};
  int64_t mailboxT4Us_ = 0;

  ClockFilter filter_;
  uint32_t samples_ = 0;
  uint32_t rejected_ = 0;
  int64_t lastDelayUs_ = -1;
  bool serverSynced_ = false;
  uint8_t serverSource_ = 0;
};

}  // namespace nhos
