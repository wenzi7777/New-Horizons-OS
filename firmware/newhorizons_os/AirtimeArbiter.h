#pragma once

#include <Arduino.h>
#include <esp_now.h>

#include "EspNowFrame.h"

namespace nhos {

// Priority classes for the one resource every ESP-NOW path competes over:
// radio airtime. Lower value wins.
enum class AirtimeClass : uint8_t {
  OtaRelay = 0,         // a firmware transfer in flight; must not be starved
  CommandResponse = 1,  // a control reply being paced out to the Hub
  Pairing = 2,          // HELLO/PAIRED/POLL handshake traffic
  SensorStream = 3,     // lossy by design, yields to everything above
  Count = 4,
};

// Single choke point for ESP-NOW transmission.
//
// Two jobs, both of which used to be hand-written if-statements:
//
// 1. Arbitration. streamingGateOk() in the sketch used to hardcode a
//    three-way exclusion between sensor streaming, the OTA relay, and
//    command responses -- every branch of it discovered the hard way on
//    real hardware. That is a priority scheduler written as conditionals,
//    so it is now a priority table.
//
//    Claimants are polled, not claim/release'd: every claimant's "am I
//    busy" state is already derived (phase_ == kRelaying, fragsSent <
//    fragCount), so asking them cannot leak or desync a claim the way a
//    scattered acquire/release pair can.
//
// 2. Pacing. Firing a multi-fragment burst's esp_now_send() calls
//    back-to-back silently drops most of them (ESP_ERR_ESPNOW_NO_MEM) --
//    a bug this project has now hit three separate times, each found only
//    on hardware. There is deliberately NO API here that takes a fragment
//    array and sends it immediately: a burst can only be handed to
//    beginPacedBurst(), so a new multi-fragment path cannot forget to
//    pace. Callers that already own a hardware-validated pacing state
//    machine (EspNowPairing's stop-and-wait retransmit,
//    EspNowStreamTransport's per-frame window) keep it and feed their
//    fragments through send() one at a time.
class AirtimeArbiter {
 public:
  using ActivePredicate = bool (*)(void* ctx);

  // Nominal window a paced burst is spread over. Matches
  // EspNowStreamTransport's own kSendWindowUs so a burst still completes
  // well inside the Hub's poll timeout.
  static constexpr uint32_t kDefaultBurstWindowUs = 15000;

  // Registers a claimant whose predicate reports whether it currently needs
  // the radio. Only classes registered here can block a lower-priority one.
  void registerClaimant(AirtimeClass cls, ActivePredicate fn, void* ctx);

  // Highest-priority class currently reporting active, or Count if idle.
  AirtimeClass holder() const;
  bool idle() const { return holder() == AirtimeClass::Count; }
  // True when nothing strictly higher-priority than `cls` is active.
  bool canClaim(AirtimeClass cls) const;

  // The single transmit path. Every esp_now_send() in the firmware goes
  // through here so airtime is accounted per class.
  esp_err_t send(AirtimeClass cls, const uint8_t* mac, const uint8_t* data, size_t len);

  // Hands a whole fragment array over to be paced out across service()
  // calls. Returns false if a burst is already draining.
  bool beginPacedBurst(AirtimeClass cls, const uint8_t* mac, const EspNowFragment* frags,
                       uint8_t count, uint32_t windowUs = kDefaultBurstWindowUs);
  // Fragments straight into the burst buffer, so a caller does not need a
  // second ~4KB scratch array just to hand the fragments over.
  bool beginPacedBurstFromBytes(AirtimeClass cls, const uint8_t* mac, const uint8_t* data,
                                size_t len, uint8_t frameType, uint16_t frameId = 0,
                                uint32_t windowUs = kDefaultBurstWindowUs);
  bool burstActive() const { return burstSent_ < burstCount_; }
  void service();

  String statusJson() const;
  String netText() const;

 private:
  struct Claimant {
    ActivePredicate fn = nullptr;
    void* ctx = nullptr;
  };
  struct Counters {
    uint32_t packets = 0;
    uint32_t bytes = 0;
    uint32_t failures = 0;
  };

  static const char* className(AirtimeClass cls);

  Claimant claimants_[static_cast<uint8_t>(AirtimeClass::Count)];
  Counters counters_[static_cast<uint8_t>(AirtimeClass::Count)];

  // One shared burst buffer for the whole firmware. ~7.7KB, so it is
  // deliberately not duplicated per caller -- EspNowOtaReceiver used to
  // carry its own static array of exactly this shape.
  EspNowFragment burstFrags_[kEspNowDataFragCount];
  uint8_t burstMac_[6] = {0};
  uint8_t burstCount_ = 0;
  uint8_t burstSent_ = 0;
  AirtimeClass burstClass_ = AirtimeClass::SensorStream;
  uint32_t burstIntervalUs_ = 0;
  uint32_t burstNextDueUs_ = 0;
};

}  // namespace nhos
