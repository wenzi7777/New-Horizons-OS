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
