#include "ClockSources.h"

#include <esp_sntp.h>
#include <esp_timer.h>
#include <sys/time.h>

#include "AirtimeArbiter.h"
#include "ClockService.h"
#include "EspNowPairing.h"
#include "UdpStreamTransport.h"

namespace {
nhos::SntpClockSource* g_sntpSource = nullptr;
}  // namespace

// Replaces ESP-IDF's weak default (esp_sntp.h: "It is possible to replace
// all SNTP update functionality by placing a sntp_sync_time() function in
// the app firmware source"). The default would settimeofday() -- a hard
// step every interval -- and bypass ClockCore's slewing entirely. Runs on
// the tcpip task.
extern "C" void sntp_sync_time(struct timeval* tv) {
  const int64_t monoUs = esp_timer_get_time();
  if (g_sntpSource != nullptr && tv != nullptr) {
    const int64_t epochUs = static_cast<int64_t>(tv->tv_sec) * 1000000LL + tv->tv_usec;
    g_sntpSource->onSntpTime(epochUs, monoUs);
  }
  sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
}

namespace nhos {

namespace {
// SNTP does its own round-trip compensation but exposes no error estimate;
// this is a conservative figure for a LAN/Internet NTP server.
constexpr uint32_t kSntpUncertaintyUs = 10000;
}  // namespace

void SntpClockSource::configure(const String& server, uint32_t intervalS) {
  const String chosen = server.isEmpty() ? String(kDefaultNtpServer) : server;
  strncpy(server_, chosen.c_str(), sizeof(server_) - 1);
  server_[sizeof(server_) - 1] = '\0';
  intervalS_ = intervalS;
  if (started_) {
    restart();
  }
}

void SntpClockSource::begin() {
  if (started_) {
    return;
  }
  if (server_[0] == '\0') {
    strncpy(server_, kDefaultNtpServer, sizeof(server_) - 1);
  }
  started_ = true;
  g_sntpSource = this;
  // Read by lwIP as SNTP_UPDATE_DELAY on every reschedule, so setting it
  // before configTime() is enough.
  sntp_set_sync_interval(intervalS_ * 1000UL);
  configTime(0, 0, server_, kFallbackNtpServer);
}

void SntpClockSource::restart() {
  if (!started_) {
    begin();
    return;
  }
  sntp_set_sync_interval(intervalS_ * 1000UL);
  // configTime() stops and re-inits the client, which both applies a new
  // server and sends a request immediately.
  configTime(0, 0, server_, kFallbackNtpServer);
}

void SntpClockSource::onSntpTime(int64_t epochUs, int64_t monoUs) {
  ClockSample sample;
  sample.source = TimeSource::Sntp;
  sample.monoUs = monoUs;
  sample.offsetUs = epochUs - monoUs;
  sample.uncertaintyUs = kSntpUncertaintyUs;
  portENTER_CRITICAL(&lock_);
  pendingSample_ = sample;
  pending_ = true;
  portEXIT_CRITICAL(&lock_);
}

void SntpClockSource::drain(ClockService& clock) {
  if (!pending_) {
    return;
  }
  ClockSample sample;
  portENTER_CRITICAL(&lock_);
  sample = pendingSample_;
  pending_ = false;
  portEXIT_CRITICAL(&lock_);
  clock.submit(sample);
  ++syncCount_;
  lastSyncMs_ = millis();
}

String SntpClockSource::statusJson() const {
  String json = "{\"started\":";
  json += started_ ? "true" : "false";
  json += ",\"server\":\"";
  json += server_;
  json += "\",\"interval_s\":";
  json += String(static_cast<unsigned long>(intervalS_));
  json += ",\"syncs\":";
  json += String(static_cast<unsigned long>(syncCount_));
  json += ",\"age_ms\":";
  json += syncCount_ != 0 ? String(millis() - lastSyncMs_) : String("null");
  json += "}";
  return json;
}

bool EspNowTimeLink::linkUp() const { return pairing_ != nullptr && pairing_->hasHub(); }

// Yields to OTA relay and command responses; the request is retried next pass.
bool EspNowTimeLink::canSendNow() const { return arbiter_ != nullptr && arbiter_->canClaim(AirtimeClass::Pairing); }

bool EspNowTimeLink::send(const uint8_t* data, size_t len) {
  return arbiter_->send(AirtimeClass::Pairing, pairing_->hubMac(), data, len) == ESP_OK;
}

bool UdpTimeLink::linkUp() const { return transport_ != nullptr && transport_->ready(); }

bool UdpTimeLink::send(const uint8_t* data, size_t len) { return transport_->sendFrame(data, len); }

void TimeExchangeSource::burst(bool reprobe) {
  if (unsupported_ && !reprobe) {
    return;
  }
  filter_.reset();
  burstRemaining_ = kBurstCount;
  nextRequestMs_ = millis();
  // An explicit sync is also the way to re-probe a server that was judged
  // too old -- it may have been updated since.
  unsupported_ = false;
  misses_ = 0;
}

void TimeExchangeSource::onResponse(const uint8_t* data, size_t len, int64_t t4MonoUs) {
  if (len != kEspNowTimeRespLen) {
    return;
  }
  portENTER_CRITICAL(&lock_);
  memcpy(mailbox_, data, len);
  mailboxT4Us_ = t4MonoUs;
  mailboxFull_ = true;
  portEXIT_CRITICAL(&lock_);
}

void TimeExchangeSource::sendRequest(uint32_t nowMs) {
  EspNowTimeRequest req;
  req.seq = ++seq_;
  uint8_t payload[kEspNowTimeReqLen];
  // Stamped as late as possible so local queueing before the radio does not
  // count as path delay.
  req.t1MonoUs = esp_timer_get_time();
  encodeEspNowTimeRequest(req, payload, sizeof(payload));
  link_->send(payload, sizeof(payload));
  awaiting_ = true;
  requestT1Us_ = req.t1MonoUs;
  requestSentMs_ = nowMs;
}

void TimeExchangeSource::handleResponse(ClockService& clock) {
  uint8_t data[kEspNowTimeRespLen];
  int64_t t4 = 0;
  portENTER_CRITICAL(&lock_);
  memcpy(data, mailbox_, sizeof(data));
  t4 = mailboxT4Us_;
  mailboxFull_ = false;
  portEXIT_CRITICAL(&lock_);

  EspNowTimeResponse resp;
  if (!decodeEspNowTimeResponse(data, sizeof(data), &resp) || !awaiting_ || resp.seq != seq_ ||
      resp.t1MonoUs != requestT1Us_) {
    return;  // a late reply to a request already given up on
  }
  awaiting_ = false;
  misses_ = 0;
  unsupported_ = false;
  answered_ = true;
  serverSynced_ = resp.hubSynced;
  serverSource_ = resp.hubSource;
  if (!resp.hubSynced) {
    ++rejected_;
    return;  // the server has no time to give yet
  }
  const EspNowTimeExchange x = computeEspNowTimeExchange(resp.t1MonoUs, resp.t2EpochUs, resp.t3EpochUs, t4);
  lastDelayUs_ = x.delayUs;
  if (x.delayUs < 0 || x.delayUs > static_cast<int64_t>(kMaxDelayUs)) {
    ++rejected_;
    return;
  }
  ClockSample sample;
  sample.source = source_;
  // The offset is that of the midpoint of the exchange.
  sample.monoUs = resp.t1MonoUs + (t4 - resp.t1MonoUs) / 2;
  sample.offsetUs = x.offsetUs;
  // Worst-case path asymmetry is half the delay; the server's own error stacks.
  sample.uncertaintyUs = static_cast<uint32_t>(x.delayUs / 2) +
                         static_cast<uint32_t>(resp.hubUncertaintyMs) * 1000UL;
  filter_.add(sample, static_cast<uint32_t>(x.delayUs));
  ++samples_;
  // Wait for the whole burst before the first sync so it can pick the best
  // round trip; afterwards every new sample re-evaluates the window.
  if (burstRemaining_ == 0 || clock.hasSynced()) {
    ClockSample best;
    if (filter_.best(&best)) {
      clock.submit(best);
    }
  }
}

void TimeExchangeSource::service(uint32_t nowMs, ClockService& clock) {
  if (!attached()) {
    return;
  }
  if (mailboxFull_) {
    handleResponse(clock);
  }
  const bool isUp = linkUp();
  if (isUp && !wasUp_) {
    answered_ = false;
    burst();  // (re)attached: the server may be a different one or freshly booted
  }
  wasUp_ = isUp;
  if (!isUp) {
    awaiting_ = false;
    return;
  }

  if (awaiting_) {
    if (nowMs - requestSentMs_ < kResponseTimeoutMs) {
      return;
    }
    awaiting_ = false;
    if (misses_ < 255) ++misses_;
    if (misses_ >= kUnsupportedAfterMisses && !answered_ && !unsupported_) {
      unsupported_ = true;
      burstRemaining_ = 0;
      nextRequestMs_ = nowMs + kUnsupportedRetryMs;
    }
  }

  if (static_cast<int32_t>(nowMs - nextRequestMs_) < 0) {
    return;
  }
  if (!link_->canSendNow()) {
    return;
  }
  sendRequest(nowMs);
  if (unsupported_) {
    nextRequestMs_ = nowMs + kUnsupportedRetryMs;
  } else if (burstRemaining_ > 0) {
    --burstRemaining_;
    nextRequestMs_ = nowMs + kBurstSpacingMs;
  } else {
    nextRequestMs_ = nowMs + intervalMs_;
  }
}

String TimeExchangeSource::statusJson() const {
  String json = "{\"link_up\":";
  json += linkUp() ? "true" : "false";
  json += ",\"supported\":";
  json += unsupported_ ? "false" : "true";
  json += ",\"server_synced\":";
  json += serverSynced_ ? "true" : "false";
  json += ",\"server_source\":\"";
  json += timeSourceName(static_cast<TimeSource>(serverSource_));
  json += "\",\"interval_s\":";
  json += String(static_cast<unsigned long>(intervalMs_ / 1000UL));
  json += ",\"samples\":";
  json += String(static_cast<unsigned long>(samples_));
  json += ",\"rejected\":";
  json += String(static_cast<unsigned long>(rejected_));
  json += ",\"misses\":";
  json += String(static_cast<unsigned int>(misses_));
  json += ",\"last_rtt_us\":";
  json += lastDelayUs_ >= 0 ? String(static_cast<long>(lastDelayUs_)) : String("null");
  json += "}";
  return json;
}

}  // namespace nhos
