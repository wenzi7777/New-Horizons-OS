#include "AppManager.h"

#include "AppEventWire.h"
#include "AppShare.h"

#include "JsonUtils.h"
#include "Storage.h"

namespace nhos {

namespace {

// Which capability bit subscribes an app to a given event kind. Budget events
// reach any app that asked for them; Custom reaches nobody automatically.
uint32_t subscriptionBit(AppEventKind kind) {
  switch (kind) {
    case AppEventKind::Frame: return kAppCapReadMatrix;
    case AppEventKind::Imu: return kAppCapReadImu;
    case AppEventKind::Tick: return kAppCapTick;
    case AppEventKind::Button: return kAppCapButton;
    case AppEventKind::Power: return kAppCapPower;
    case AppEventKind::Link: return kAppCapLink;
    case AppEventKind::Budget: return kAppCapBudget;
    case AppEventKind::Custom:
    default: return 0;
  }
}

void padLeft(String& out, const String& value, unsigned width) {
  for (unsigned i = value.length(); i < width; ++i) out += ' ';
  out += value;
}

void padRight(String& out, const String& value, unsigned width) {
  out += value;
  for (unsigned i = value.length(); i < width; ++i) out += ' ';
}

}  // namespace

// Running AND with something to run. An app counted here while it had nothing
// to run would take a share of the budget it cannot use.
bool AppManager::active(const App* app) {
  return app->run.state == AppState::Running && !app->idle();
}

bool AppManager::add(App* app, uint16_t order) {
  if (app == nullptr || app->manifest().name == nullptr || app->manifest().name[0] == '\0') {
    return false;
  }
  for (App* it = head_; it != nullptr; it = it->run.next) {
    if (it == app || strcmp(it->manifest().name, app->manifest().name) == 0) return false;
  }
  app->run = AppRun();
  app->run.order = order;
  // Activated is running. While the governor has every app suspended, this one
  // joins them and comes back with them.
  if (throttled_ && totalBudgetUs_ == 0) {
    app->run.state = AppState::Suspended;
  } else {
    app->run.state = app->start() ? AppState::Running : AppState::Faulted;
  }
  App** link = &head_;
  while (*link != nullptr && (*link)->run.order <= order) link = &(*link)->run.next;
  app->run.next = *link;
  *link = app;
  ++count_;
  reallocate();
  return app->run.state != AppState::Faulted;
}

void AppManager::remove(App* app) {
  for (App** link = &head_; *link != nullptr; link = &(*link)->run.next) {
    if (*link != app) continue;
    *link = app->run.next;
    app->run.next = nullptr;
    --count_;
    reallocate();
    return;
  }
}

void AppManager::sortByOrder() {
  App* pending = head_;
  head_ = nullptr;
  while (pending != nullptr) {
    App* app = pending;
    pending = pending->run.next;
    App** link = &head_;
    while (*link != nullptr && (*link)->run.order <= app->run.order) link = &(*link)->run.next;
    app->run.next = *link;
    *link = app;
  }
}

App* AppManager::find(const String& name) const {
  for (App* app = head_; app != nullptr; app = app->run.next) {
    if (name == app->manifest().name) return app;
  }
  return nullptr;
}

uint16_t AppManager::runningCount() const {
  uint16_t running = 0;
  for (const App* app = head_; app != nullptr; app = app->run.next) {
    if (active(app)) ++running;
  }
  return running;
}

bool AppManager::revive(const String& name) {
  App* app = find(name);
  if (app == nullptr) {
    return false;
  }
  app->run.consecutiveOverruns = 0;
  app->run.state = app->start() ? AppState::Running : AppState::Faulted;
  reallocate();
  return app->run.state == AppState::Running;
}

AppManager::OpResult AppManager::stop(const String& name) {
  App* app = find(name);
  if (app == nullptr) {
    return OpResult::UnknownApp;
  }
  if (app->run.state != AppState::Running && app->run.state != AppState::Suspended) {
    return OpResult::NotRunning;
  }
  if (app->run.state == AppState::Running) {
    app->stop();
  }
  app->run.state = AppState::Stopped;
  recordEvent(app->manifest().name, "stopped", "operator", false, 0);
  reallocate();
  return OpResult::Ok;
}

AppManager::OpResult AppManager::start(const String& name) {
  App* app = find(name);
  if (app == nullptr) {
    return OpResult::UnknownApp;
  }
  if (app->run.state != AppState::Stopped) {
    return OpResult::NotStopped;
  }
  app->run.consecutiveOverruns = 0;
  if (throttled_ && totalBudgetUs_ == 0) {
    // The governor has every app suspended right now; join them, and come
    // back with them when the scan recovers.
    app->run.state = AppState::Suspended;
    return OpResult::Ok;
  }
  app->run.state = app->start() ? AppState::Running : AppState::Faulted;
  reallocate();
  return app->run.state == AppState::Running ? OpResult::Ok : OpResult::StartFailed;
}

const char* AppManager::opResultCode(OpResult result) {
  switch (result) {
    case OpResult::Ok: return "ok";
    case OpResult::UnknownApp: return "unknown_app";
    case OpResult::NotRunning: return "app_not_running";
    case OpResult::NotStopped: return "app_not_stopped";
    case OpResult::StartFailed: return "app_start_failed";
    default: return "unknown";
  }
}

void AppManager::setTotalBudgetUs(uint32_t totalUs, bool throttled) {
  throttled_ = throttled;
  if (totalUs == totalBudgetUs_) {
    return;
  }
  totalBudgetUs_ = totalUs;
  // The yardstick moved, so the tally of consecutive overruns against the old
  // one means nothing. Without this an app is killed for the first few frames
  // after a cut it had no part in.
  for (App* app = head_; app != nullptr; app = app->run.next) {
    app->run.consecutiveOverruns = 0;
  }
  if (totalUs == 0) {
    // The scanner needs everything. Suspend rather than kill: this is not the
    // apps' fault and they must come back by themselves.
    for (App* app = head_; app != nullptr; app = app->run.next) {
      if (app->run.state == AppState::Running) {
        app->stop();
        app->run.state = AppState::Suspended;
        recordEvent(app->manifest().name, "suspended", "scan_pressure", false, 0);
      }
    }
    reallocate();
    return;
  }
  for (App* app = head_; app != nullptr; app = app->run.next) {
    if (app->run.state == AppState::Suspended) {
      app->run.state = app->start() ? AppState::Running : AppState::Faulted;
      if (app->run.state == AppState::Running) {
        recordEvent(app->manifest().name, "resumed", "scan_recovered", false, 0);
      }
    }
  }
  reallocate();
}

// The guaranteed shares only: every loan is dropped, and the next settle
// lends again from these.
void AppManager::reallocate() {
  const uint16_t running = runningCount();
  activeCount_ = running;
  const uint32_t base = AppShare::base(totalBudgetUs_, running);
  for (App* app = head_; app != nullptr; app = app->run.next) {
    const bool on = active(app);
    app->run.baseUs = on ? base : 0;
    app->run.allocatedUs = on ? base : 0;
    app->run.borrowedUs = 0;
    app->run.lentUs = 0;
  }
}

// Once a second: who used how much, and who lends spare time to whom. Every
// app's window is read here before AppLoad::settle() clears it.
void AppManager::settle(uint32_t nowMs) {
  const uint32_t nowUs = micros();
  const uint32_t wallUs = lastSettleUs_ != 0 ? nowUs - lastSettleUs_ : kSettleMs * 1000U;
  lastSettleUs_ = nowUs;
  lastSettleMs_ = nowMs;

  const uint16_t running = runningCount();
  const uint32_t base = AppShare::base(totalBudgetUs_, running);
  const uint32_t cap = AppShare::cap(totalBudgetUs_, running);
  uint32_t pool = 0;
  uint16_t borrowers = 0;
  for (App* app = head_; app != nullptr; app = app->run.next) {
    AppRun& run = app->run;
    const uint32_t pressure = app->pressureCount();
    const uint32_t pressureEvents = pressure - run.lastPressure;
    run.lastPressure = pressure;
    const uint32_t average = run.windowDeliveries != 0 ? run.load.windowUsedUs / run.windowDeliveries : 0;
    run.windowPressured = pressureEvents != 0;
    run.load.settle(wallUs, pressureEvents);
    run.windowDeliveries = 0;

    // Lending is decided here too, from the same window. lentUs holds what the
    // app could lend, and borrowedUs marks a borrower, until the second pass.
    run.lentUs = 0;
    run.borrowedUs = 0;
    if (!active(app) || base == 0) continue;
    const bool hungry = AppShare::hungry(average, run.allocatedUs, run.windowPressured);
    if (app->manifest().burst && hungry) {
      run.borrowedUs = 1;
      ++borrowers;
    } else {
      run.lentUs = AppShare::lendable(base, average, hungry);
      pool += run.lentUs;
    }
  }

  const uint32_t loan = AppShare::loan(pool, borrowers, base, cap);
  const uint32_t borrowed = loan * borrowers;
  for (App* app = head_; app != nullptr; app = app->run.next) {
    AppRun& run = app->run;
    if (!active(app) || base == 0) {
      run.lentUs = 0;
      run.borrowedUs = 0;
      continue;
    }
    run.baseUs = base;
    if (run.borrowedUs != 0) {
      run.borrowedUs = loan;
      run.lentUs = 0;
    } else {
      run.lentUs = AppShare::lent(run.lentUs, borrowed, pool);
    }
    run.allocatedUs = base - run.lentUs + run.borrowedUs;
  }
}

void AppManager::warnOverBudget(App& app, uint32_t elapsedUs, uint32_t nowMs) {
  const AppManifest& manifest = app.manifest();
  AppRun& run = app.run;
  const uint8_t graceLeft = static_cast<uint8_t>(
      kMaxConsecutiveOverruns > run.consecutiveOverruns
          ? kMaxConsecutiveOverruns - run.consecutiveOverruns
          : 0);
  const uint32_t allocated = run.allocatedUs != 0 ? run.allocatedUs : manifest.frameBudgetUs;

  // The operator hears about it either way -- they have levers the app does
  // not (stop something else, change the app, raise the ceiling).
  recordEvent(manifest.name, "over_budget",
              String("us=") + String(elapsedUs) + " of " + String(allocated) +
                  " grace=" + String(graceLeft),
              true, allocated > 0 ? static_cast<float>(elapsedUs) / allocated : 0.0f);

  // And so does the app, if it asked -- it may shed work, or it may ignore
  // this and be stopped. That is its choice, not ours.
  if ((manifest.capabilities & kAppCapBudget) != 0) {
    AppEvent warning;
    warning.kind = AppEventKind::Budget;
    warning.nowMs = nowMs;
    warning.frameSeq = frameSeq_;
    warning.host = this;
    warning.load = allocated > 0 ? static_cast<float>(elapsedUs) / allocated : 0.0f;
    warning.graceLeft = graceLeft;
    app.onEvent(warning);
  }
}

void AppManager::service(uint32_t nowMs) {
  // Every listed app, running or not: one that was just stopped may still
  // have state to write out, and that write is exactly what service() is for.
  for (App* app = head_; app != nullptr; app = app->run.next) {
    app->service(nowMs);
  }
  if (lastSettleMs_ == 0 || nowMs - lastSettleMs_ >= kSettleMs) {
    settle(nowMs);
  }
}

void AppManager::dispatch(const AppEvent& event) {
  ++dispatches_;
  if (event.kind == AppEventKind::Frame) {
    frameSeq_ = event.frameSeq;
  }
  // An app faulting, or the registry adding or removing one, changes how many
  // share the budget; noticing it here keeps the split right without wiring
  // every path.
  if (runningCount() != activeCount_) {
    reallocate();
  }
  const uint32_t required = subscriptionBit(event.kind);
  uint32_t totalUs = 0;

  for (App* app = head_; app != nullptr; app = app->run.next) {
    AppRun& run = app->run;
    if (!active(app)) {
      continue;
    }
    const AppManifest& manifest = app->manifest();
    // Subscription and permission are the same bit, so an app that may not
    // read the matrix is simply never woken for a frame.
    if (required == 0 || (manifest.capabilities & required) == 0) {
      continue;
    }

    AppEvent delivered = event;
    // Capability enforcement is here, not inside the app: an app that never
    // declared kAppCapReadMatrix is not handed a frame even by accident.
    if ((manifest.capabilities & kAppCapReadMatrix) == 0) {
      delivered.frame = nullptr;
    }
    if ((manifest.capabilities & kAppCapReadImu) == 0) {
      delivered.imuSample = nullptr;
    }
    if ((manifest.capabilities & kAppCapReadMag) == 0) {
      delivered.magSample = nullptr;
    }
    if ((manifest.capabilities & kAppCapPower) == 0) {
      delivered.batteryPercent = -1.0f;
    }
    if ((manifest.capabilities & kAppCapLink) == 0) {
      delivered.linked = false;
    }
    delivered.host = this;
    const uint32_t slice = run.allocatedUs != 0 ? run.allocatedUs : manifest.frameBudgetUs;
    delivered.sliceUs = slice;

    const uint32_t startedUs = micros();
    app->onEvent(delivered);
    const uint32_t elapsedUs = micros() - startedUs;
    totalUs += elapsedUs;

    ++run.invocations;
    run.lastUs = elapsedUs;
    if (elapsedUs > run.maxUs) {
      run.maxUs = elapsedUs;
    }

    // Budget events are the runtime talking to the app about its own cost;
    // charging that conversation against the same budget would be circular.
    if (event.kind == AppEventKind::Budget) {
      continue;
    }
    run.load.charge(elapsedUs, slice);
    ++run.windowDeliveries;

    // Judged against the guaranteed share, not a lender's reduced slice: an
    // app that lent time and needs it back is not at fault for the second it
    // takes to get it.
    const uint32_t allowed = run.baseUs > slice ? run.baseUs : slice;
    if (allowed != 0 && elapsedUs > allowed) {
      ++run.overruns;
      ++run.consecutiveOverruns;
      warnOverBudget(*app, elapsedUs, event.nowMs);
      if (run.consecutiveOverruns >= kMaxConsecutiveOverruns) {
        // Not preemption -- the frame it just cost is already gone. But it
        // bounds the damage to a handful of frames instead of every frame
        // for the rest of the session.
        app->stop();
        // Whose fault was it? Under throttling the app is being judged against
        // an allocation the governor just cut, so this is the system's doing
        // and must clear itself. Killing would make a transient scan overload
        // permanently disable an app until someone noticed and revived it.
        run.state = throttled_ ? AppState::Suspended : AppState::Killed;
        recordEvent(manifest.name, throttled_ ? "suspended" : "killed",
                    String("last_us=") + String(elapsedUs), false, 0);
        if (storage_ != nullptr) {
          storage_->logTagged("apps",
                              String(throttled_ ? "app_suspended name=" : "app_killed name=") +
                                  manifest.name + " last_us=" + String(elapsedUs) +
                                  " budget_us=" + String(allowed),
                              LogLevel::Warn);
        }
        reallocate();
      }
    } else {
      run.consecutiveOverruns = 0;
    }
  }

  lastTotalUs_ = totalUs;
  if (totalUs > maxTotalUs_) {
    maxTotalUs_ = totalUs;
  }
}

void AppManager::recordEvent(const char* app, const char* event, const String& detail,
                             bool hasValue, float value) {
  EventRecord& record = events_[eventWrite_];
  if (record.valid) {
    // The ring wrapped before anyone read this record. Counted, because a lost
    // event and an event that never happened must not look the same.
    ++eventsDropped_;
  }
  record.valid = true;
  record.seq = ++eventSeq_;
  record.ms = millis();
  record.frameSeq = frameSeq_;
  record.hasValue = hasValue;
  record.value = value;
  strncpy(record.app, app != nullptr ? app : "", sizeof(record.app) - 1);
  record.app[sizeof(record.app) - 1] = '\0';
  strncpy(record.event, event != nullptr ? event : "", sizeof(record.event) - 1);
  record.event[sizeof(record.event) - 1] = '\0';
  strncpy(record.detail, detail.c_str(), sizeof(record.detail) - 1);
  record.detail[sizeof(record.detail) - 1] = '\0';
  eventWrite_ = static_cast<uint8_t>((eventWrite_ + 1) % kEventLogEntries);
}

void AppManager::emitEvent(const char* app, const char* event, const String& detail) {
  App* source = app != nullptr ? find(String(app)) : nullptr;
  if (source != nullptr) {
    if ((source->manifest().capabilities & kAppCapEmitEvent) == 0) {
      return;
    }
    ++source->run.events;
  }
  recordEvent(app, event, detail, false, 0);
}

void AppManager::emitValue(const char* app, const char* event, float value) {
  App* source = app != nullptr ? find(String(app)) : nullptr;
  if (source != nullptr) {
    if ((source->manifest().capabilities & kAppCapEmitEvent) == 0) {
      return;
    }
    ++source->run.events;
  }
  recordEvent(app, event, String(value, 3), true, value);
}

namespace {

void appendName(String& out, bool& first, const App* app) {
  out += first ? "[\"" : ",\"";
  first = false;
  out += jsonEscape(String(app->manifest().name));
  out += "\"";
}

}  // namespace

// Pulled, not pushed. Until v1.7.0 the led op wrote the pixel directly and the
// LED task painted the system pattern back over it within 10ms, so an app's
// colour was all but invisible, and two apps raced with the later one winning.
// Composing here gives the LED the same first-in-order rule as the OLED and
// the strip, and an app that stops holds nothing because it is not running.
bool AppManager::statusLed(uint8_t rgb[3], const App** owner, String* others) const {
  const App* winner = nullptr;
  bool first = true;
  for (const App* app = head_; app != nullptr; app = app->run.next) {
    if (!active(app) || (app->manifest().capabilities & kAppCapDriveLed) == 0) {
      continue;
    }
    uint8_t held[3];
    if (!app->statusLed(held)) {
      continue;
    }
    if (winner == nullptr) {
      winner = app;
      rgb[0] = held[0];
      rgb[1] = held[1];
      rgb[2] = held[2];
      if (others == nullptr) break;
      continue;
    }
    appendName(*others, first, app);
  }
  if (others != nullptr && !first) *others += "]";
  if (owner != nullptr) *owner = winner;
  return winner != nullptr;
}

bool AppManager::displayLine(uint8_t row, AppDisplayLine& out, const App** owner) const {
  for (const App* app = head_; app != nullptr; app = app->run.next) {
    if (!active(app) || (app->manifest().capabilities & kAppCapDisplay) == 0) {
      continue;
    }
    if (app->displayLine(row, out)) {
      if (owner != nullptr) *owner = app;
      return true;
    }
  }
  return false;
}

bool AppManager::appendDisplayContenders(String& out, uint8_t row, const App* winner) const {
  bool first = true;
  for (const App* app = head_; app != nullptr; app = app->run.next) {
    if (app == winner || !active(app) || (app->manifest().capabilities & kAppCapDisplay) == 0) {
      continue;
    }
    AppDisplayLine line;
    if (app->displayLine(row, line)) appendName(out, first, app);
  }
  if (!first) out += "]";
  return !first;
}

void AppManager::extLedFrame(AppExtLedFrame& out) const {
  out = AppExtLedFrame();
  for (const App* app = head_; app != nullptr; app = app->run.next) {
    if (!active(app) || (app->manifest().capabilities & kAppCapExtLed) == 0) {
      continue;
    }
    out.active = true;
    app->extLedFrame(out);
  }
}

void AppManager::publish(const char* app, uint32_t topic, float value) {
  for (App* target = head_; target != nullptr; target = target->run.next) {
    if (!active(target)) continue;
    if (app != nullptr && strcmp(target->manifest().name, app) == 0) continue;
    target->queueMessage(topic, value);
  }
}

void AppManager::logLine(const char* app, const String& line) {
  if (storage_ == nullptr) {
    return;
  }
  storage_->logTagged("apps", String(app) + ": " + line, LogLevel::Info);
}

const char* AppManager::stateName(AppState state) {
  switch (state) {
    case AppState::Running: return "running";
    case AppState::Killed: return "killed";
    case AppState::Faulted: return "faulted";
    case AppState::Suspended: return "suspended";
    case AppState::Stopped: return "stopped";
    case AppState::Installed:
    default: return "installed";
  }
}

namespace {

void appendLoad(String& json, const AppRun& run) {
  json += ",\"load\":";
  json += String(run.load.load);
  json += ",\"level\":\"";
  json += AppLoad::levelName(run.load.level);
  json += "\",\"cpu1\":";
  json += String(run.load.cpu1);
  json += ",\"cpu10\":";
  json += String(run.load.cpu10);
  json += ",\"cpu60\":";
  json += String(run.load.cpu60);
  json += ",\"budget10\":";
  json += String(run.load.budget10);
  json += ",\"base_us\":";
  json += String(run.baseUs);
  json += ",\"borrowed_us\":";
  json += String(run.borrowedUs);
  json += ",\"lent_us\":";
  json += String(run.lentUs);
}

}  // namespace

String AppManager::statusJson(const String& outputsFor) const {
  String json = "{\"dispatches\":";
  json += String(dispatches_);
  json += ",\"count\":";
  json += String(count_);
  json += ",\"running\":";
  json += String(runningCount());
  json += ",\"budget_us\":";
  json += String(totalBudgetUs_);
  json += ",\"last_total_us\":";
  json += String(lastTotalUs_);
  json += ",\"max_total_us\":";
  json += String(maxTotalUs_);
  json += ",\"event_seq\":";
  json += String(eventSeq_);
  json += ",\"events_dropped\":";
  json += String(eventsDropped_);
  json += ",\"apps\":[";
  bool first = true;
  for (const App* app = head_; app != nullptr; app = app->run.next) {
    const AppRun& run = app->run;
    const AppManifest& manifest = app->manifest();
    if (!first) json += ",";
    first = false;
    json += "{\"name\":\"";
    json += jsonEscape(String(manifest.name));
    json += "\",\"version\":\"";
    json += jsonEscape(String(manifest.version));
    json += "\",\"order\":";
    json += String(run.order);
    json += ",\"state\":\"";
    json += stateName(run.state);
    json += "\",\"idle\":";
    json += app->idle() ? "true" : "false";
    json += ",\"burst\":";
    json += manifest.burst ? "true" : "false";
    json += ",\"capabilities\":";
    json += String(manifest.capabilities);
    json += ",\"budget_us\":";
    // An idle app holds no share; showing the manifest default would claim
    // budget it does not have.
    json += String(app->idle() ? 0 : run.allocatedUs != 0 ? run.allocatedUs : manifest.frameBudgetUs);
    appendLoad(json, run);
    json += ",\"memory_bytes\":";
    json += String(app->memoryBytes());
    json += ",\"invocations\":";
    json += String(run.invocations);
    json += ",\"last_us\":";
    json += String(run.lastUs);
    json += ",\"max_us\":";
    json += String(run.maxUs);
    json += ",\"overruns\":";
    json += String(run.overruns);
    json += ",\"events\":";
    json += String(run.events);
    json += ",\"state_detail\":";
    json += app->statusJson(outputsFor.length() > 0 && outputsFor == manifest.name);
    json += "}";
  }
  json += "]}";
  return json;
}

String AppManager::loadJson() const {
  uint32_t cpu = 0;
  String apps = "[";
  bool first = true;
  for (const App* app = head_; app != nullptr; app = app->run.next) {
    const AppRun& run = app->run;
    cpu += run.load.cpu10;
    if (!first) apps += ",";
    first = false;
    apps += "{\"name\":\"";
    apps += jsonEscape(String(app->manifest().name));
    apps += "\",\"state\":\"";
    apps += app->idle() && run.state == AppState::Running ? "faulted" : stateName(run.state);
    apps += "\"";
    appendLoad(apps, run);
    apps += ",\"memory_bytes\":";
    apps += String(app->memoryBytes());
    apps += "}";
  }
  apps += "]";
  String json = "{\"budget_us\":";
  json += String(totalBudgetUs_);
  json += ",\"throttled\":";
  json += throttled_ ? "true" : "false";
  json += ",\"cpu10\":";
  json += String(cpu > 1000 ? 1000 : cpu);
  json += ",\"apps\":";
  json += apps;
  json += "}";
  return json;
}

size_t AppManager::takeStreamEvents(uint8_t* out, size_t capacity) {
  if (out == nullptr || eventSeq_ == streamedSeq_) {
    return 0;
  }
  if (eventSeq_ < streamedSeq_) {
    streamedSeq_ = 0;  // never expected: the sequence only grows until reboot
  }
  size_t written = 0;
  for (uint8_t i = 0; i < kEventLogEntries; ++i) {
    // Oldest first, as in eventsJson().
    const EventRecord& record = events_[(eventWrite_ + i) % kEventLogEntries];
    if (!record.valid || record.seq <= streamedSeq_) {
      continue;
    }
    AppEventFields fields;
    fields.seq = record.seq;
    fields.frameSeq = record.frameSeq;
    fields.uptimeMs = record.ms;
    fields.hasValue = record.hasValue;
    fields.value = record.value;
    fields.app = record.app;
    fields.event = record.event;
    fields.detail = record.detail;
    const size_t length = encodeAppEventTlv(fields, out + written, capacity - written);
    if (length == 0) {
      break;  // full: this one and the rest go in the next packet
    }
    written += length;
    streamedSeq_ = record.seq;
  }
  return written;
}

String AppManager::eventsJson(uint32_t sinceSeq, uint8_t limit) const {
  if (limit == 0 || limit > kEventLogEntries) {
    limit = kEventLogEntries;
  }
  String json = "{\"seq\":";
  json += String(eventSeq_);
  // Events also ride the stream packets (takeStreamEvents()), so a receiver
  // needs this only to fill gaps, not to poll.
  json += ",\"streaming\":true";
  json += ",\"dropped\":";
  json += String(eventsDropped_);
  json += ",\"events\":[";
  uint8_t emitted = 0;
  for (uint8_t i = 0; i < kEventLogEntries && emitted < limit; ++i) {
    // Oldest first, so a caller polling by sequence gets them in order.
    const EventRecord& record = events_[(eventWrite_ + i) % kEventLogEntries];
    if (!record.valid || record.seq <= sinceSeq) {
      continue;
    }
    if (emitted != 0) {
      json += ",";
    }
    json += "{\"seq\":";
    json += String(record.seq);
    json += ",\"ms\":";
    json += String(record.ms);
    json += ",\"frame_seq\":";
    json += String(record.frameSeq);
    json += ",\"app\":\"";
    json += jsonEscape(String(record.app));
    json += "\",\"event\":\"";
    json += jsonEscape(String(record.event));
    json += "\",\"detail\":\"";
    json += jsonEscape(String(record.detail));
    json += "\"";
    if (record.hasValue) {
      json += ",\"value\":";
      json += String(record.value, 3);
    }
    json += "}";
    ++emitted;
  }
  json += "]}";
  return json;
}

String AppManager::appsText() const {
  String out = "NAME             STATE       BUDGET_US  LAST_US   MAX_US  OVERRUNS  CPU%  LOAD\n";
  for (const App* app = head_; app != nullptr; app = app->run.next) {
    const AppRun& run = app->run;
    padRight(out, String(app->manifest().name), 17);
    padRight(out, String(run.state == AppState::Running && app->idle() ? "faulted" : stateName(run.state)),
             11);
    padLeft(out, String(app->idle() ? 0 : run.allocatedUs != 0 ? run.allocatedUs
                                                                 : app->manifest().frameBudgetUs),
            10);
    padLeft(out, String(run.lastUs), 9);
    padLeft(out, String(run.maxUs), 9);
    padLeft(out, String(run.overruns), 10);
    padLeft(out, String(run.load.cpu10 / 10.0f, 1), 6);
    padLeft(out, String(run.load.load), 6);
    out += '\n';
  }
  out += "\nTOTAL budget_us=";
  out += String(totalBudgetUs_);
  out += " last_us=";
  out += String(lastTotalUs_);
  out += " max_us=";
  out += String(maxTotalUs_);
  out += " dropped_events=";
  out += String(eventsDropped_);
  out += "\n\nEVENTS\n";
  for (uint8_t i = 0; i < kEventLogEntries; ++i) {
    const EventRecord& record = events_[(eventWrite_ + i) % kEventLogEntries];
    if (!record.valid) {
      continue;
    }
    out += "[";
    out += String(record.seq);
    out += " @";
    out += String(record.ms);
    out += " f";
    out += String(record.frameSeq);
    out += "] ";
    out += record.app;
    out += " ";
    out += record.event;
    out += " ";
    out += record.detail;
    out += '\n';
  }
  return out;
}

}  // namespace nhos
