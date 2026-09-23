#pragma once

#include <Arduino.h>

#include "App.h"

namespace nhos {

class Storage;

enum class AppState : uint8_t {
  Installed = 0,  // present but not running
  Running = 1,
  Killed = 2,     // repeatedly exceeded its own allocation -- the app's fault
  Faulted = 3,    // start() refused
  // Stopped because the system needed the capacity, NOT because the app did
  // anything wrong. Distinct from Killed because it must come back on its own:
  // a suspension that needed an operator to clear it would mean one busy
  // moment takes an app away permanently, with nothing saying why.
  Suspended = 4,
};

// Installs, dispatches and contains apps.
//
// Two containment rules, and they answer different questions. Per app: every
// call is timed against its allocation, and kMaxConsecutiveOverruns overruns
// in a row take it out of the dispatch list. System-wide: AppGovernor watches
// whether the SCANNER is meeting its deadlines and shrinks what the apps may
// spend, down to nothing if that is what it takes.
//
// Scanning always wins. The device exists to sample the matrix; apps are value
// added on top of that, never at its expense.
class AppManager : public AppHost {
 public:
  static constexpr uint8_t kMaxApps = 8;
  static constexpr uint8_t kMaxConsecutiveOverruns = 5;
  // 32 rather than 8: at two events a second the old ring wrapped before the
  // Desktop could poll it, and a lost event is indistinguishable from one that
  // never happened. 32 is ~16s of headroom against a sub-second poll, which is
  // ample -- 64 would have cost 6.8KB of static RAM for margin nobody uses.
  static constexpr uint8_t kEventLogEntries = 32;

  void begin(Storage* storage) { storage_ = storage; }

  // Installed apps are DISABLED unless a previous app_enable persisted
  // otherwise. Deliberate: an app costs budget on every dispatch, and silently
  // adding per-frame work to a measurement instrument on upgrade is not a
  // decision this framework gets to make for the operator.
  bool install(App* app);
  bool setEnabled(const String& name, bool enabled);
  // Clears a kill verdict and restarts. The operator's override, same shape
  // as ServiceManager::restart(). Does not apply to Suspended -- that clears
  // itself.
  bool revive(const String& name);

  // Dispatches one event to every running app subscribed to its kind.
  void dispatch(const AppEvent& event);

  // --- allocation, driven by AppGovernor -----------------------------------
  // Sets the total the apps may spend per dispatch and divides it among the
  // running ones. Zero suspends them all.
  //
  // `throttled` says the governor has cut this below its ceiling. While that
  // holds, an app that overruns is SUSPENDED rather than killed: it is being
  // measured against a yardstick the system just moved, and killing it would
  // need an operator to undo something the app did not do.
  void setTotalBudgetUs(uint32_t totalUs, bool throttled = false);
  uint32_t totalBudgetUs() const { return totalBudgetUs_; }
  uint8_t runningCount() const;
  uint32_t lastTotalUs() const { return lastTotalUs_; }
  uint32_t maxTotalUs() const { return maxTotalUs_; }

  // AppHost
  void emitEvent(const char* app, const char* event, const String& detail) override;
  void emitValue(const char* app, const char* event, float value) override;
  void setLed(const char* app, uint8_t r, uint8_t g, uint8_t b) override;
  void logLine(const char* app, const String& line) override;

  // The OLED row the display should draw, composed across slots: the
  // lowest-numbered running slot that drew `row` on its last frame wins, so
  // two apps can share the panel by using different rows. Capability-checked
  // here rather than in the app, like every other AppHost entry point.
  bool displayLine(uint8_t row, AppDisplayLine& out) const;

  using LedSink = void (*)(uint8_t r, uint8_t g, uint8_t b);
  void setLedSink(LedSink sink) { ledSink_ = sink; }

  static const char* stateName(AppState state);
  // Per-node outputs are included only for the slot named by `outputsFor`
  // (or none): four slots of 24 nodes each would not fit an ESP-NOW reply.
  String statusJson(const String& outputsFor = String()) const;
  String appsText() const;
  // Events after `sinceSeq`, plus how many were overwritten before being read.
  String eventsJson(uint32_t sinceSeq, uint8_t limit) const;
  uint32_t eventSeq() const { return eventSeq_; }

 private:
  struct Slot {
    App* app = nullptr;
    AppState state = AppState::Installed;
    uint32_t invocations = 0;
    uint32_t lastUs = 0;
    uint32_t maxUs = 0;
    uint32_t overruns = 0;
    uint8_t consecutiveOverruns = 0;
    uint32_t events = 0;
    // Allocation this slot currently holds, as set by setTotalBudgetUs().
    uint32_t allocatedUs = 0;
  };

  struct EventRecord {
    bool valid = false;
    uint32_t seq = 0;
    uint32_t ms = 0;
    uint32_t frameSeq = 0;
    bool hasValue = false;
    float value = 0;
    char app[16] = {0};
    char event[24] = {0};
    char detail[48] = {0};
  };

  int8_t indexOf(const String& name) const;
  int8_t indexOfApp(const char* name) const;
  static String enabledKey(const char* name);
  void recordEvent(const char* app, const char* event, const String& detail,
                   bool hasValue, float value);
  void reallocate();
  void warnOverBudget(Slot& slot, uint32_t elapsedUs, uint32_t nowMs);

  Slot slots_[kMaxApps];
  uint8_t count_ = 0;
  // Which running slots had work at the last dispatch; a change (a package
  // bound or removed) re-divides the budget.
  uint8_t activeMask_ = 0;
  uint8_t activeMask() const;
  Storage* storage_ = nullptr;
  EventRecord events_[kEventLogEntries];
  uint8_t eventWrite_ = 0;
  uint32_t eventSeq_ = 0;
  uint32_t eventsDropped_ = 0;
  uint32_t dispatches_ = 0;
  uint32_t frameSeq_ = 0;
  uint32_t totalBudgetUs_ = 0;
  bool throttled_ = false;
  uint32_t lastTotalUs_ = 0;
  uint32_t maxTotalUs_ = 0;
  LedSink ledSink_ = nullptr;
};

}  // namespace nhos
