#pragma once

namespace nhos {

// Loop-task watchdog, wrapped so feeding is safe from code that runs both
// during setup() and from loop().
//
// The naked Arduino feedLoopWDT() calls esp_task_wdt_reset() on whatever task
// is current. Before enableLoopWDT() has subscribed the loop task that returns
// ESP_ERR_NOT_FOUND and the IDF logs "task not found" -- at 20Hz from
// WifiManager's 8s association loop, which is enough to flood the boot log off
// the wire entirely (found on real hardware: every boot stage after
// magnetometer_ready was lost). Both of the long loops that feed the watchdog
// also run during setup(), so the guard has to live here rather than at each
// call site.
void watchdogArm();
void watchdogDisarm();
bool watchdogArmed();
void watchdogFeed();

// Scoped disarm for a single blocking call that cannot be fed from inside.
//
// HTTPClient::GET() and Update::end() are one long call each, not loops, so
// there is nowhere to put a feed -- and they legitimately exceed the 5s
// timeout (the manifest fetch alone allows 6s connect + 8s read). Before the
// watchdog existed these were merely slow; with it armed they reboot the
// device mid-OTA-check, so the pause is required rather than a nicety.
//
// Safe because every call it wraps carries its own timeout, so the window is
// bounded rather than open-ended. RAII because these functions are full of
// early returns.
class WatchdogPause {
 public:
  WatchdogPause() : wasArmed_(watchdogArmed()) {
    if (wasArmed_) {
      watchdogDisarm();
    }
  }
  ~WatchdogPause() {
    if (wasArmed_) {
      watchdogArm();
    }
  }
  WatchdogPause(const WatchdogPause&) = delete;
  WatchdogPause& operator=(const WatchdogPause&) = delete;

 private:
  bool wasArmed_;
};

}  // namespace nhos
