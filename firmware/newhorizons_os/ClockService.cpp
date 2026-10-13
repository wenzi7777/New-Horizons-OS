#include "ClockService.h"

#include <esp_timer.h>
#include <sys/time.h>

#include "DeviceConfig.h"

namespace nhos {

namespace {
// How long after boot an unsynced clock still counts as "starting up"
// rather than unhealthy.
constexpr uint32_t kClockHealthGraceMs = 120000;
}  // namespace

int64_t ClockService::monoUs() { return esp_timer_get_time(); }

void ClockService::configure(const DeviceConfig& config) {
  const ClockConfig& c = config.data().clock;
  sntp_.configure(c.ntpServer, c.ntpIntervalS);
  hub_.setIntervalS(c.syncIntervalS);
  gateway_.setIntervalS(c.syncIntervalS);
}

void ClockService::setEspNowMode(bool espNow) { espNowMode_ = espNow; }

void ClockService::beginSntp() {
  if (!espNowMode_) {
    sntp_.begin();
  }
}

void ClockService::attachHub(EspNowPairing* pairing, AirtimeArbiter* arbiter) {
  hubLink_.attach(pairing, arbiter);
  hub_.attach(&hubLink_);
}

void ClockService::attachGateway(UdpStreamTransport* transport) {
  gatewayLink_.attach(transport);
  gateway_.attach(&gatewayLink_);
}

void ClockService::service(uint32_t nowMs) {
  sntp_.drain(*this);
  hub_.service(nowMs, *this);
  gateway_.service(nowMs, *this);
}

bool ClockService::requestSync(bool reprobe) {
  if (hub_.attached()) {
    hub_.burst(reprobe);
    return true;
  }
  // WiFi: ask the Gateway and NTP both; whichever answers, ClockCore's
  // priority keeps the Gateway in charge when it is there.
  bool asked = false;
  if (gateway_.linkUp()) {
    gateway_.burst(reprobe);
    asked = true;
  }
  if (sntp_.started()) {
    sntp_.restart();
    asked = true;
  }
  return asked;
}

uint64_t ClockService::nowEpochMs() const { return epochMsAt(monoUs()); }

uint64_t ClockService::epochMsAt(int64_t mono) const {
  const int64_t epochUs = core_.epochUsAt(mono);
  return epochUs > 0 ? static_cast<uint64_t>(epochUs / 1000) : 0;
}

uint32_t ClockService::uncertaintyMs() const {
  const uint32_t us = core_.uncertaintyUsAt(monoUs());
  return us == UINT32_MAX ? UINT32_MAX : (us + 999) / 1000;
}

ClockSubmitResult ClockService::setEpochMs(uint64_t epochMs, TimeSource source, bool force) {
  const int64_t now = monoUs();
  ClockSample sample;
  sample.source = source;
  sample.monoUs = now;
  sample.offsetUs = static_cast<int64_t>(epochMs) * 1000 - now;
  // A pushed value carries however long it spent in transit, unmeasured.
  sample.uncertaintyUs = 500000;
  return submit(sample, force);
}

ClockSubmitResult ClockService::submit(const ClockSample& sample, bool force) {
  const int64_t now = monoUs();
  const ClockSubmitResult result = core_.submit(sample, now, force);
  lastResult_ = result;
  lastResultMs_ = millis();
  if (result == ClockSubmitResult::Stepped) {
    // Keep libc's clock roughly in line for anything that calls time();
    // packet timestamps never read it.
    const int64_t epochUs = core_.epochUsAt(now);
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(epochUs / 1000000LL);
    tv.tv_usec = static_cast<suseconds_t>(epochUs % 1000000LL);
    settimeofday(&tv, nullptr);
  }
  return result;
}

bool ClockService::healthy() const {
  if (core_.synced()) {
    return true;
  }
  if (espNowMode_) {
    // An old Hub (no time service) is reported in status.clock.hub, not as
    // ill health: a service restart cannot fix the Hub's firmware, and the
    // restart hook's re-sync would defeat the source's 5-minute re-probe.
    // Not paired yet is likewise the transport's problem, not the clock's.
    return !hub_.supported() || !hub_.linkUp() || millis() < kClockHealthGraceMs;
  }
  // WiFi: unhealthy only when neither the Gateway nor NTP has delivered;
  // not connected yet (SNTP not started) is the transport's problem.
  return !sntp_.started() || millis() < kClockHealthGraceMs;
}

String ClockService::statusJson() const {
  const int64_t now = monoUs();
  const bool synced = core_.synced();
  String json = "{\"source\":\"";
  json += timeSourceName(core_.source());
  json += "\",\"synced\":";
  json += synced ? "true" : "false";
  json += ",\"uptime_ms\":";
  json += String(millis());
  json += ",\"epoch_ms\":";
  json += synced ? String(static_cast<unsigned long long>(epochMsAt(now))) : String("null");
  json += ",\"age_ms\":";
  json += synced ? String(static_cast<unsigned long>((now - core_.lastSyncMonoUs()) / 1000)) : String("null");
  json += ",\"uncertainty_ms\":";
  json += synced ? String(static_cast<unsigned long>(uncertaintyMs())) : String("null");
  json += ",\"last_adjust_ms\":";
  json += String(static_cast<long long>(lastAdjustMs()));
  json += ",\"pending_slew_ms\":";
  json += String(static_cast<long>(core_.pendingSlewUsAt(now) / 1000));
  json += ",\"adjust_count\":";
  json += String(static_cast<unsigned long>(core_.adjustCount()));
  json += ",\"step_count\":";
  json += String(static_cast<unsigned long>(core_.stepCount()));
  json += ",\"last_result\":\"";
  json += lastResultMs_ != 0 ? clockSubmitResultName(lastResult_) : "none";
  json += "\",\"mode\":\"";
  json += espNowMode_ ? "espnow" : "wifi";
  json += "\"";
  if (!espNowMode_) {
    json += ",\"gateway\":";
    json += gateway_.statusJson();
    json += ",\"sntp\":";
    json += sntp_.statusJson();
  }
  if (espNowMode_) {
    json += ",\"hub\":";
    json += hub_.statusJson();
  }
  json += "}";
  return json;
}

}  // namespace nhos
