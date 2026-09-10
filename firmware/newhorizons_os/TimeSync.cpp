#include "TimeSync.h"

#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include "Config.h"

namespace nhos {

namespace {
TimeSync* g_activeInstance = nullptr;

void onSntpSynced(struct timeval* /*tv*/) {
  if (g_activeInstance) {
    g_activeInstance->recordSync();
  }
}
}  // namespace

void TimeSync::begin() {
  if (started_) {
    return;
  }
  started_ = true;
  g_activeInstance = this;
  sntp_set_time_sync_notification_cb(onSntpSynced);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
}

void TimeSync::recordSync() {
  synced_ = true;
  source_ = TimeSource::Sntp;
  lastSetMs_ = millis();
}

const char* TimeSync::sourceName(TimeSource source) {
  switch (source) {
    case TimeSource::Sntp: return "sntp";
    case TimeSource::Host: return "host";
    case TimeSource::None:
    default: return "none";
  }
}

bool TimeSync::setEpochMs(uint64_t epochMs, TimeSource source) {
  if (epochMs / 1000ULL < static_cast<uint64_t>(kTimeSyncValidEpochS)) {
    return false;
  }
  // Measure before stepping, so the reported adjustment is the error this
  // push corrected rather than zero.
  if (hasSynced()) {
    lastAdjustMs_ = static_cast<int64_t>(epochMs) - static_cast<int64_t>(nowEpochMs());
  } else {
    lastAdjustMs_ = 0;
  }
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(epochMs / 1000ULL);
  tv.tv_usec = static_cast<suseconds_t>((epochMs % 1000ULL) * 1000ULL);
  if (settimeofday(&tv, nullptr) != 0) {
    return false;
  }
  synced_ = true;
  source_ = source;
  lastSetMs_ = millis();
  ++adjustCount_;
  return true;
}

String TimeSync::statusJson() const {
  String json = "{\"source\":\"";
  json += sourceName(source_);
  json += "\",\"synced\":";
  json += hasSynced() ? "true" : "false";
  json += ",\"uptime_ms\":";
  json += String(millis());
  json += ",\"epoch_ms\":";
  json += hasSynced() ? String(static_cast<unsigned long long>(nowEpochMs())) : String("null");
  json += ",\"age_ms\":";
  json += lastSetMs_ != 0 ? String(millis() - lastSetMs_) : String("null");
  json += ",\"last_adjust_ms\":";
  json += String(static_cast<long long>(lastAdjustMs_));
  json += ",\"adjust_count\":";
  json += String(adjustCount_);
  json += "}";
  return json;
}

bool TimeSync::hasSynced() const {
  if (synced_) {
    return true;
  }
  // Fall back to checking the system clock directly in case the callback was
  // missed (e.g. begin() called after a sync already happened internally).
  return time(nullptr) >= static_cast<time_t>(kTimeSyncValidEpochS);
}

uint64_t TimeSync::nowEpochMs() const {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

}  // namespace nhos
