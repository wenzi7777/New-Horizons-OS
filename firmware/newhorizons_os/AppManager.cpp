#include "AppManager.h"

#include "JsonUtils.h"
#include "Storage.h"

namespace nhos {

namespace {

// Which capability bit subscribes an app to a given event kind. Budget events
// reach any app that asked for them; Custom reaches nobody automatically.
uint16_t subscriptionBit(AppEventKind kind) {
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

}  // namespace

bool AppManager::install(App* app) {
  if (count_ >= kMaxApps || app == nullptr || app->manifest().name == nullptr) {
    return false;
  }
  if (indexOf(String(app->manifest().name)) >= 0) {
    return false;
  }
  Slot& slot = slots_[count_++];
  slot.app = app;
  slot.state = AppState::Installed;
  if (storage_ != nullptr &&
      storage_->getUInt(enabledKey(app->manifest().name).c_str(), 0) != 0) {
    slot.state = app->start() ? AppState::Running : AppState::Faulted;
  }
  reallocate();
  return slot.state != AppState::Faulted;
}

String AppManager::enabledKey(const char* name) {
  // NVS keys cap at 15 chars, so the prefix stays short.
  return String("app_en_") + String(name);
}

int8_t AppManager::indexOf(const String& name) const {
  for (uint8_t i = 0; i < count_; ++i) {
    if (name == slots_[i].app->manifest().name) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

int8_t AppManager::indexOfApp(const char* name) const {
  if (name == nullptr) {
    return -1;
  }
  for (uint8_t i = 0; i < count_; ++i) {
    if (strcmp(name, slots_[i].app->manifest().name) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

// Running AND with something to run. An empty slot counted here took an equal
// share of the budget: one real app beside three empty slots got a quarter.
uint8_t AppManager::runningCount() const {
  uint8_t running = 0;
  for (uint8_t i = 0; i < count_; ++i) {
    if (slots_[i].state == AppState::Running && !slots_[i].app->idle()) {
      ++running;
    }
  }
  return running;
}

uint8_t AppManager::activeMask() const {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < count_ && i < 8; ++i) {
    if (slots_[i].state == AppState::Running && !slots_[i].app->idle()) {
      mask |= static_cast<uint8_t>(1u << i);
    }
  }
  return mask;
}

bool AppManager::setEnabled(const String& name, bool enabled) {
  const int8_t index = indexOf(name);
  if (index < 0) {
    return false;
  }
  Slot& slot = slots_[index];
  if (storage_ != nullptr) {
    // Persisted, so an operator's choice survives a reboot the way a service
    // enable would.
    storage_->putUInt(enabledKey(slot.app->manifest().name).c_str(), enabled ? 1 : 0);
  }
  if (!enabled) {
    if (slot.state == AppState::Running) {
      slot.app->stop();
    }
    slot.state = AppState::Installed;
    reallocate();
    return true;
  }
  // A killed app is not silently re-enabled: that verdict is cleared only by
  // revive(), so an operator has to acknowledge it.
  if (slot.state == AppState::Killed) {
    return false;
  }
  slot.state = slot.app->start() ? AppState::Running : AppState::Faulted;
  reallocate();
  return slot.state == AppState::Running;
}

bool AppManager::revive(const String& name) {
  const int8_t index = indexOf(name);
  if (index < 0) {
    return false;
  }
  Slot& slot = slots_[index];
  slot.consecutiveOverruns = 0;
  slot.state = slot.app->start() ? AppState::Running : AppState::Faulted;
  reallocate();
  return slot.state == AppState::Running;
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
  for (uint8_t i = 0; i < count_; ++i) {
    slots_[i].consecutiveOverruns = 0;
  }
  if (totalUs == 0) {
    // The scanner needs everything. Suspend rather than kill: this is not the
    // apps' fault and they must come back by themselves.
    for (uint8_t i = 0; i < count_; ++i) {
      if (slots_[i].state == AppState::Running) {
        slots_[i].app->stop();
        slots_[i].state = AppState::Suspended;
        recordEvent(slots_[i].app->manifest().name, "suspended", "scan_pressure", false, 0);
      }
    }
    reallocate();
    return;
  }
  for (uint8_t i = 0; i < count_; ++i) {
    if (slots_[i].state == AppState::Suspended) {
      slots_[i].state = slots_[i].app->start() ? AppState::Running : AppState::Faulted;
      if (slots_[i].state == AppState::Running) {
        recordEvent(slots_[i].app->manifest().name, "resumed", "scan_recovered", false, 0);
      }
    }
  }
  reallocate();
}

void AppManager::reallocate() {
  const uint8_t running = runningCount();
  const uint32_t share = running > 0 ? totalBudgetUs_ / running : 0;
  for (uint8_t i = 0; i < count_; ++i) {
    slots_[i].allocatedUs =
        slots_[i].state == AppState::Running && !slots_[i].app->idle() ? share : 0;
  }
}

void AppManager::warnOverBudget(Slot& slot, uint32_t elapsedUs, uint32_t nowMs) {
  const AppManifest& manifest = slot.app->manifest();
  const uint8_t graceLeft = static_cast<uint8_t>(
      kMaxConsecutiveOverruns > slot.consecutiveOverruns
          ? kMaxConsecutiveOverruns - slot.consecutiveOverruns
          : 0);
  const uint32_t allocated = slot.allocatedUs != 0 ? slot.allocatedUs : manifest.frameBudgetUs;

  // The operator hears about it either way -- they have levers the app does
  // not (disable something else, edit the graph, raise the ceiling).
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
    slot.app->onEvent(warning);
  }
}

void AppManager::dispatch(const AppEvent& event) {
  ++dispatches_;
  if (event.kind == AppEventKind::Frame) {
    frameSeq_ = event.frameSeq;
  }
  // Binding and removing packages happens in the registry, which does not
  // tell us; noticing it here keeps the split right without wiring every path.
  const uint8_t mask = activeMask();
  if (mask != activeMask_) {
    activeMask_ = mask;
    reallocate();
  }
  const uint16_t required = subscriptionBit(event.kind);
  uint32_t totalUs = 0;

  for (uint8_t i = 0; i < count_; ++i) {
    Slot& slot = slots_[i];
    if (slot.state != AppState::Running || slot.app->idle()) {
      continue;
    }
    const AppManifest& manifest = slot.app->manifest();
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
    delivered.host = this;

    const uint32_t startedUs = micros();
    slot.app->onEvent(delivered);
    const uint32_t elapsedUs = micros() - startedUs;
    totalUs += elapsedUs;

    ++slot.invocations;
    slot.lastUs = elapsedUs;
    if (elapsedUs > slot.maxUs) {
      slot.maxUs = elapsedUs;
    }

    // Budget events are the runtime talking to the app about its own cost;
    // charging that conversation against the same budget would be circular.
    if (event.kind == AppEventKind::Budget) {
      continue;
    }

    const uint32_t allocated = slot.allocatedUs != 0 ? slot.allocatedUs : manifest.frameBudgetUs;
    if (allocated != 0 && elapsedUs > allocated) {
      ++slot.overruns;
      ++slot.consecutiveOverruns;
      warnOverBudget(slot, elapsedUs, event.nowMs);
      if (slot.consecutiveOverruns >= kMaxConsecutiveOverruns) {
        // Not preemption -- the frame it just cost is already gone. But it
        // bounds the damage to a handful of frames instead of every frame
        // for the rest of the session.
        slot.app->stop();
        // Whose fault was it? Under throttling the app is being judged against
        // an allocation the governor just cut, so this is the system's doing
        // and must clear itself. Killing would make a transient scan overload
        // permanently disable an app until someone noticed and revived it.
        slot.state = throttled_ ? AppState::Suspended : AppState::Killed;
        recordEvent(manifest.name, throttled_ ? "suspended" : "killed",
                    String("last_us=") + String(elapsedUs), false, 0);
        if (storage_ != nullptr) {
          storage_->logTagged("apps",
                              String(throttled_ ? "app_suspended name=" : "app_killed name=") +
                                  manifest.name + " last_us=" + String(elapsedUs) +
                                  " budget_us=" + String(allocated),
                              LogLevel::Warn);
        }
        reallocate();
      }
    } else {
      slot.consecutiveOverruns = 0;
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
    // The ring wrapped before anyone read this slot. Counted, because a lost
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
  const int8_t index = indexOfApp(app);
  if (index >= 0) {
    if ((slots_[index].app->manifest().capabilities & kAppCapEmitEvent) == 0) {
      return;
    }
    ++slots_[index].events;
  }
  recordEvent(app, event, detail, false, 0);
}

void AppManager::emitValue(const char* app, const char* event, float value) {
  const int8_t index = indexOfApp(app);
  if (index >= 0) {
    if ((slots_[index].app->manifest().capabilities & kAppCapEmitEvent) == 0) {
      return;
    }
    ++slots_[index].events;
  }
  recordEvent(app, event, String(value, 3), true, value);
}

void AppManager::setLed(const char* app, uint8_t r, uint8_t g, uint8_t b) {
  const int8_t index = indexOfApp(app);
  if (index < 0 || (slots_[index].app->manifest().capabilities & kAppCapDriveLed) == 0) {
    return;
  }
  if (ledSink_ != nullptr) {
    ledSink_(r, g, b);
  }
}

bool AppManager::displayLine(uint8_t row, AppDisplayLine& out) const {
  for (uint8_t i = 0; i < count_; ++i) {
    const Slot& slot = slots_[i];
    if (slot.state != AppState::Running || slot.app->idle() ||
        (slot.app->manifest().capabilities & kAppCapDisplay) == 0) {
      continue;
    }
    if (slot.app->displayLine(row, out)) {
      return true;
    }
  }
  return false;
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
    case AppState::Installed:
    default: return "installed";
  }
}

String AppManager::statusJson(const String& outputsFor) const {
  String json = "{\"dispatches\":";
  json += String(dispatches_);
  json += ",\"count\":";
  json += String(count_);
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
  for (uint8_t i = 0; i < count_; ++i) {
    const Slot& slot = slots_[i];
    const AppManifest& manifest = slot.app->manifest();
    if (i != 0) {
      json += ",";
    }
    json += "{\"name\":\"";
    json += jsonEscape(String(manifest.name));
    json += "\",\"version\":\"";
    json += jsonEscape(String(manifest.version));
    json += "\",\"state\":\"";
    json += stateName(slot.state);
    json += "\",\"idle\":";
    json += slot.app->idle() ? "true" : "false";
    json += ",\"capabilities\":";
    json += String(manifest.capabilities);
    json += ",\"budget_us\":";
    // An idle slot holds no share; showing the manifest default would claim
    // budget it does not have.
    json += String(slot.app->idle() ? 0
                   : slot.allocatedUs != 0 ? slot.allocatedUs : manifest.frameBudgetUs);
    json += ",\"invocations\":";
    json += String(slot.invocations);
    json += ",\"last_us\":";
    json += String(slot.lastUs);
    json += ",\"max_us\":";
    json += String(slot.maxUs);
    json += ",\"overruns\":";
    json += String(slot.overruns);
    json += ",\"events\":";
    json += String(slot.events);
    json += ",\"state_detail\":";
    json += slot.app->statusJson(outputsFor.length() > 0 && outputsFor == manifest.name);
    json += "}";
  }
  json += "]}";
  return json;
}

String AppManager::eventsJson(uint32_t sinceSeq, uint8_t limit) const {
  if (limit == 0 || limit > kEventLogEntries) {
    limit = kEventLogEntries;
  }
  String json = "{\"seq\":";
  json += String(eventSeq_);
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
  String out = "NAME              STATE      BUDGET_US  LAST_US   MAX_US  OVERRUNS\n";
  for (uint8_t i = 0; i < count_; ++i) {
    const Slot& slot = slots_[i];
    String name(slot.app->manifest().name);
    while (name.length() < 17) {
      name += ' ';
    }
    out += name;
    String state(slot.state == AppState::Running && slot.app->idle() ? "idle"
                                                                     : stateName(slot.state));
    while (state.length() < 11) {
      state += ' ';
    }
    out += state;
    String budget(slot.app->idle() ? 0
                  : slot.allocatedUs != 0 ? slot.allocatedUs
                                          : slot.app->manifest().frameBudgetUs);
    while (budget.length() < 10) {
      budget = " " + budget;
    }
    out += budget;
    String last(slot.lastUs);
    while (last.length() < 9) {
      last = " " + last;
    }
    out += last;
    String maxUs(slot.maxUs);
    while (maxUs.length() < 9) {
      maxUs = " " + maxUs;
    }
    out += maxUs;
    String overruns(slot.overruns);
    while (overruns.length() < 10) {
      overruns = " " + overruns;
    }
    out += overruns;
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
