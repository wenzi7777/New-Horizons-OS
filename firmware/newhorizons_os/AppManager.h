#pragma once

#include <Arduino.h>

#include "App.h"

namespace nhos {

class Storage;

enum class AppState : uint8_t {
  Installed = 0,  // present but not running
  Running = 1,
  Killed = 2,     // exceeded its frame budget too many times in a row
  Faulted = 3,    // start() refused
};

// Installs, dispatches and contains apps.
//
// The containment rule is the whole point: every onFrame() is timed against
// the app's declared budget, and kMaxConsecutiveOverruns overruns in a row
// take it out of the dispatch list. Without that, one badly written app
// silently costs scan frames -- which on a research instrument means corrupted
// data rather than a slow UI, and is exactly why this could not have been
// built before the scheduler existed to measure anything.
class AppManager : public AppHost {
 public:
  static constexpr uint8_t kMaxApps = 8;
  static constexpr uint8_t kMaxConsecutiveOverruns = 5;
  static constexpr uint8_t kEventLogEntries = 8;

  void begin(Storage* storage) { storage_ = storage; }

  // Installed apps are DISABLED unless a previous app_enable persisted
  // otherwise. Deliberate: an app costs frame budget on every scan, and
  // silently adding per-frame work to a measurement instrument on upgrade is
  // not a decision this framework gets to make for the operator -- the same
  // reason PowerGovernor defaults to `performance`.
  bool install(App* app);
  bool setEnabled(const String& name, bool enabled);
  // Clears a kill verdict and restarts. The operator's override, same shape
  // as ServiceManager::restart().
  bool revive(const String& name);

  // Dispatches one scanned frame to every running app.
  void onFrame(const MatrixFrame& frame, const float* imuSample, uint32_t nowMs);

  // AppHost
  void emitEvent(const char* app, const char* event, const String& detail) override;
  void logLine(const char* app, const String& line) override;

  static const char* stateName(AppState state);
  String statusJson() const;
  String appsText() const;

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
  };

  struct EventRecord {
    bool valid = false;
    uint32_t ms = 0;
    char app[16] = {0};
    char event[24] = {0};
    char detail[48] = {0};
  };

  int8_t indexOf(const String& name) const;
  static String enabledKey(const char* name);

  Slot slots_[kMaxApps];
  uint8_t count_ = 0;
  Storage* storage_ = nullptr;
  EventRecord events_[kEventLogEntries];
  uint8_t eventWrite_ = 0;
  uint32_t frames_ = 0;
};

}  // namespace nhos
