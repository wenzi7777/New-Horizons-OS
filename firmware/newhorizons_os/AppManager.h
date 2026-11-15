#pragma once

#include <Arduino.h>

#include "App.h"

namespace nhos {

class Storage;

// Dispatches and contains the running apps.
//
// Two containment rules, and they answer different questions. Per app: every
// call is timed against its allocation, and kMaxConsecutiveOverruns overruns
// in a row take it out of the dispatch list. System-wide: AppGovernor watches
// whether the SCANNER is meeting its deadlines and shrinks what the apps may
// spend, down to nothing if that is what it takes.
//
// Scanning always wins. The device exists to sample the matrix; apps are value
// added on top of that, never at its expense.
//
// v2.1.0: no fixed slots. The run list holds every app the registry has
// activated, however many fit the AppPool; each app carries its own record
// (App::run), kept in `order` -- the operator's ordering, which also decides
// which app's output wins where they compete (OLED rows, LED, strip).
class AppManager : public AppHost {
 public:
  static constexpr uint8_t kMaxConsecutiveOverruns = 5;
  // 32 rather than 8: at two events a second the old ring wrapped before the
  // Desktop could poll it, and a lost event is indistinguishable from one that
  // never happened. 32 is ~16s of headroom against a sub-second poll, which is
  // ample -- 64 would have cost 6.8KB of static RAM for margin nobody uses.
  static constexpr uint8_t kEventLogEntries = 32;
  // How often load is settled and spare time re-lent (AppLoad, AppShare).
  static constexpr uint32_t kSettleMs = 1000;

  void begin(Storage* storage) { storage_ = storage; }

  // Puts an activated app in the run list and starts it -- or, while the
  // governor has every app suspended, joins them. False if `app` is null,
  // unnamed or already listed.
  bool add(App* app, uint16_t order);
  // Takes an app out of the run list (deactivation, uninstall). Does not stop
  // or free it: the registry does both, after this.
  void remove(App* app);
  // Re-sorts the list after the registry renumbered App::run.order.
  void sortByOrder();

  // Clears a kill verdict and restarts. The operator's override, same shape
  // as ServiceManager::restart(). Does not apply to Suspended -- that clears
  // itself.
  bool revive(const String& name);
  // Runtime-only stop/start (app_stop / app_start): the "end task" of an app.
  // stop() applies to a Running or Suspended app; start() only to one that
  // stop() stopped -- anything else goes through app_revive.
  enum class OpResult : uint8_t { Ok = 0, UnknownApp, NotRunning, NotStopped, StartFailed };
  OpResult stop(const String& name);
  OpResult start(const String& name);
  static const char* opResultCode(OpResult result);

  // Dispatches one event to every running app subscribed to its kind.
  void dispatch(const AppEvent& event);
  // Lets every app do its untimed housekeeping (App::service), and once a
  // second settles the load and re-lends spare time.
  void service(uint32_t nowMs);

  // --- allocation, driven by AppGovernor -----------------------------------
  // Sets the total the apps may spend per dispatch and divides it among the
  // running ones. Zero suspends them all. Any change drops every loan: the
  // next settle lends again from the new shares.
  //
  // `throttled` says the governor has cut this below its ceiling. While that
  // holds, an app that overruns is SUSPENDED rather than killed: it is being
  // measured against a yardstick the system just moved, and killing it would
  // need an operator to undo something the app did not do.
  void setTotalBudgetUs(uint32_t totalUs, bool throttled = false);
  uint32_t totalBudgetUs() const { return totalBudgetUs_; }
  uint16_t runningCount() const;
  uint16_t count() const { return count_; }
  uint32_t lastTotalUs() const { return lastTotalUs_; }
  uint32_t maxTotalUs() const { return maxTotalUs_; }
  App* find(const String& name) const;
  App* first() const { return head_; }

  // AppHost
  void emitEvent(const char* app, const char* event, const String& detail) override;
  void emitValue(const char* app, const char* event, float value) override;
  void logLine(const char* app, const String& line) override;
  // app.publish: queued for every other running app (each keeps
  // kVmMessageQueue; an app's on("message") handlers run on its next
  // delivery, inside its own budget).
  void publish(const char* app, uint32_t topic, float value) override;

  // The OLED row the display should draw, composed across apps: the first
  // running app in `order` that drew `row` on its last frame wins, so two
  // apps can share the panel by using different rows. Capability-checked
  // here rather than in the app, like every other AppHost entry point.
  bool displayLine(uint8_t row, AppDisplayLine& out, const App** owner = nullptr) const;
  // Appends a JSON array of the other running apps that drew `row` too.
  // Returns false (appending nothing) when there are none. For app_view.
  bool appendDisplayContenders(String& out, uint8_t row, const App* winner) const;

  // What the external LED strip shows, composed across apps the same way:
  // the first app's meter, and for each pixel the first app that lit it.
  // `out.active` is set while any running app may drive the strip, whether
  // or not it lit anything.
  void extLedFrame(AppExtLedFrame& out) const;

  // The status LED colour the apps are asking for, composed the same way:
  // the first running app in order holding one wins. The system still decides
  // whether it is shown (LedController::appMayOverride). `others`, when
  // given, gets a JSON array of the other apps holding a colour (or nothing).
  bool statusLed(uint8_t rgb[3], const App** owner = nullptr, String* others = nullptr) const;

  static const char* stateName(AppState state);
  // Per-node outputs are included only for the app named by `outputsFor`
  // (or none): every app's would not fit an ESP-NOW reply.
  String statusJson(const String& outputsFor = String()) const;
  // Load and memory per app, compact enough for app_view's frequent polls.
  String loadJson() const;
  String appsText() const;
  // Events after `sinceSeq`, plus how many were overwritten before being read.
  String eventsJson(uint32_t sinceSeq, uint8_t limit) const;
  uint32_t eventSeq() const { return eventSeq_; }
  // Events not yet sent in a stream packet, oldest first, encoded as
  // app_event extension TLVs (AppEventWire.h) into at most `capacity` bytes;
  // the rest wait for the next packet. Returns the bytes written. A packet
  // that is then lost loses them from the stream only: the ring still has
  // them for app_events, which is how a receiver fills the gap.
  size_t takeStreamEvents(uint8_t* out, size_t capacity);

 private:
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

  static bool active(const App* app);
  void recordEvent(const char* app, const char* event, const String& detail,
                   bool hasValue, float value);
  void reallocate();
  // Settles every app's load window and re-lends spare time (AppShare).
  void settle(uint32_t nowMs);
  void warnOverBudget(App& app, uint32_t elapsedUs, uint32_t nowMs);

  App* head_ = nullptr;
  uint16_t count_ = 0;
  // How many apps had work at the last dispatch; a change (one faulted, one
  // was added or removed) re-divides the budget.
  uint16_t activeCount_ = 0;
  Storage* storage_ = nullptr;
  EventRecord events_[kEventLogEntries];
  uint8_t eventWrite_ = 0;
  uint32_t eventSeq_ = 0;
  // Highest event sequence already handed to a stream packet.
  uint32_t streamedSeq_ = 0;
  uint32_t eventsDropped_ = 0;
  uint32_t dispatches_ = 0;
  uint32_t frameSeq_ = 0;
  uint32_t totalBudgetUs_ = 0;
  bool throttled_ = false;
  uint32_t lastTotalUs_ = 0;
  uint32_t maxTotalUs_ = 0;
  uint32_t lastSettleMs_ = 0;
  uint32_t lastSettleUs_ = 0;
};

}  // namespace nhos
