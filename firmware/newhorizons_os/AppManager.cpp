#include "AppManager.h"

#include "JsonUtils.h"
#include "Storage.h"

namespace nhos {

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
    return true;
  }
  // A killed app is not silently re-enabled: that verdict is cleared only by
  // revive(), so an operator has to acknowledge it.
  if (slot.state == AppState::Killed) {
    return false;
  }
  slot.state = slot.app->start() ? AppState::Running : AppState::Faulted;
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
  return slot.state == AppState::Running;
}

void AppManager::onFrame(const MatrixFrame& frame, const float* imuSample, uint32_t nowMs) {
  ++frames_;
  for (uint8_t i = 0; i < count_; ++i) {
    Slot& slot = slots_[i];
    if (slot.state != AppState::Running) {
      continue;
    }
    const AppManifest& manifest = slot.app->manifest();

    AppFrameContext context;
    // Capability enforcement is here, not inside the app: an app that never
    // declared kAppCapReadMatrix simply is not handed one.
    context.frame = (manifest.capabilities & kAppCapReadMatrix) != 0 ? &frame : nullptr;
    context.imuSample = (manifest.capabilities & kAppCapReadImu) != 0 ? imuSample : nullptr;
    context.nowMs = nowMs;
    context.host = this;

    const uint32_t startedUs = micros();
    slot.app->onFrame(context);
    const uint32_t elapsedUs = micros() - startedUs;

    ++slot.invocations;
    slot.lastUs = elapsedUs;
    if (elapsedUs > slot.maxUs) {
      slot.maxUs = elapsedUs;
    }
    if (manifest.frameBudgetUs != 0 && elapsedUs > manifest.frameBudgetUs) {
      ++slot.overruns;
      if (++slot.consecutiveOverruns >= kMaxConsecutiveOverruns) {
        // Not preemption -- the frame it just cost is already gone. But it
        // bounds the damage to a handful of frames instead of every frame
        // for the rest of the session.
        slot.app->stop();
        slot.state = AppState::Killed;
        if (storage_ != nullptr) {
          storage_->logTagged("apps",
                              String("app_killed name=") + manifest.name + " last_us=" +
                                  String(elapsedUs) + " budget_us=" +
                                  String(manifest.frameBudgetUs),
                              LogLevel::Warn);
        }
      }
    } else {
      slot.consecutiveOverruns = 0;
    }
  }
}

void AppManager::emitEvent(const char* app, const char* event, const String& detail) {
  const int8_t index = indexOf(String(app));
  if (index >= 0) {
    if ((slots_[index].app->manifest().capabilities & kAppCapEmitEvent) == 0) {
      return;
    }
    ++slots_[index].events;
  }
  EventRecord& record = events_[eventWrite_];
  record.valid = true;
  record.ms = millis();
  strncpy(record.app, app, sizeof(record.app) - 1);
  record.app[sizeof(record.app) - 1] = '\0';
  strncpy(record.event, event, sizeof(record.event) - 1);
  record.event[sizeof(record.event) - 1] = '\0';
  strncpy(record.detail, detail.c_str(), sizeof(record.detail) - 1);
  record.detail[sizeof(record.detail) - 1] = '\0';
  eventWrite_ = static_cast<uint8_t>((eventWrite_ + 1) % kEventLogEntries);
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
    case AppState::Installed:
    default: return "installed";
  }
}

String AppManager::statusJson() const {
  String json = "{\"frames\":";
  json += String(frames_);
  json += ",\"count\":";
  json += String(count_);
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
    json += "\",\"capabilities\":";
    json += String(manifest.capabilities);
    json += ",\"budget_us\":";
    json += String(manifest.frameBudgetUs);
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
    json += slot.app->statusJson();
    json += "}";
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
    String state(stateName(slot.state));
    while (state.length() < 11) {
      state += ' ';
    }
    out += state;
    String budget(slot.app->manifest().frameBudgetUs);
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
  out += "\nEVENTS\n";
  for (uint8_t i = 0; i < kEventLogEntries; ++i) {
    const EventRecord& record = events_[(eventWrite_ + i) % kEventLogEntries];
    if (!record.valid) {
      continue;
    }
    out += "[";
    out += String(record.ms);
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
