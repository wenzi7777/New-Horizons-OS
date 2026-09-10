#include "Scheduler.h"

#include "JsonUtils.h"

namespace nhos {

bool Scheduler::registerTask(const char* name, TaskFn fn, bool alwaysRun, uint32_t periodUs) {
  if (count_ >= kMaxTasks || name == nullptr || fn == nullptr || indexOf(name) >= 0) {
    return false;
  }
  Task& task = tasks_[count_++];
  task.name = name;
  task.fn = fn;
  task.periodUs = periodUs;
  task.nextDueUs = micros();
  task.alwaysRun = alwaysRun;
  task.enabled = true;
  return true;
}

int8_t Scheduler::indexOf(const char* name) const {
  for (uint8_t i = 0; i < count_; ++i) {
    if (strcmp(tasks_[i].name, name) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

bool Scheduler::setEnabled(const char* name, bool enabled) {
  const int8_t index = indexOf(name);
  if (index < 0) {
    return false;
  }
  tasks_[index].enabled = enabled;
  return true;
}

void Scheduler::tick() {
  const uint32_t tickStartUs = micros();
  for (uint8_t i = 0; i < count_; ++i) {
    Task& task = tasks_[i];
    if (!task.enabled) {
      continue;
    }
    // Lazily, not once per tick: the gate depends on state that earlier
    // tasks in this same pass may have just changed.
    if (!task.alwaysRun && gate_ != nullptr && !gate_()) {
      continue;
    }
    const uint32_t nowUs = micros();
    if (task.periodUs != 0 && static_cast<int32_t>(nowUs - task.nextDueUs) < 0) {
      continue;
    }
    task.fn();
    const uint32_t elapsedUs = micros() - nowUs;
    if (task.periodUs != 0) {
      task.nextDueUs = nowUs + task.periodUs;
    }
    TaskStats& stats = task.stats;
    ++stats.invocations;
    stats.lastUs = elapsedUs;
    stats.totalUs += elapsedUs;
    stats.windowUs += elapsedUs;
    if (elapsedUs > stats.maxUs) {
      stats.maxUs = elapsedUs;
    }
  }

  const uint32_t tickUs = micros() - tickStartUs;
  ++ticks_;
  if (tickUs > maxTickUs_) {
    maxTickUs_ = tickUs;
  }
  windowBusyUs_ += tickUs;
  closeCpuWindowIfDue(tickStartUs);
}

void Scheduler::closeCpuWindowIfDue(uint32_t nowUs) {
  if (windowStartedUs_ == 0) {
    windowStartedUs_ = nowUs;
    return;
  }
  const uint32_t windowUs = nowUs - windowStartedUs_;
  if (windowUs < kCpuWindowUs) {
    return;
  }
  for (uint8_t i = 0; i < count_; ++i) {
    TaskStats& stats = tasks_[i].stats;
    stats.cpuPermille = static_cast<uint16_t>((stats.windowUs * 1000ULL) / windowUs);
    stats.windowUs = 0;
  }
  busyPermille_ = static_cast<uint16_t>((static_cast<uint64_t>(windowBusyUs_) * 1000ULL) / windowUs);
  windowBusyUs_ = 0;
  windowStartedUs_ = nowUs;
}

String Scheduler::statusJson() const {
  String json = "{\"ticks\":";
  json += String(ticks_);
  json += ",\"max_tick_us\":";
  json += String(maxTickUs_);
  json += ",\"busy_permille\":";
  json += String(busyPermille_);
  json += ",\"task_count\":";
  json += String(count_);
  json += ",\"tasks\":[";
  for (uint8_t i = 0; i < count_; ++i) {
    const Task& task = tasks_[i];
    if (i != 0) {
      json += ",";
    }
    json += "{\"name\":\"";
    json += jsonEscape(String(task.name));
    json += "\",\"enabled\":";
    json += task.enabled ? "true" : "false";
    json += ",\"always_run\":";
    json += task.alwaysRun ? "true" : "false";
    json += ",\"period_us\":";
    json += String(task.periodUs);
    json += ",\"invocations\":";
    json += String(task.stats.invocations);
    json += ",\"last_us\":";
    json += String(task.stats.lastUs);
    json += ",\"max_us\":";
    json += String(task.stats.maxUs);
    json += ",\"cpu_permille\":";
    json += String(task.stats.cpuPermille);
    json += "}";
  }
  json += "]}";
  return json;
}

String Scheduler::tasksText() const {
  String out;
  out.reserve(128 + count_ * 64);
  out += "NAME              RUN   INVOCATIONS   LAST_US    MAX_US   CPU%\n";
  for (uint8_t i = 0; i < count_; ++i) {
    const Task& task = tasks_[i];
    String name(task.name);
    while (name.length() < 17) {
      name += ' ';
    }
    out += name;
    out += ' ';
    out += task.enabled ? (task.alwaysRun ? "all" : "run") : "off";
    String invocations(task.stats.invocations);
    while (invocations.length() < 14) {
      invocations = " " + invocations;
    }
    out += invocations;
    String last(task.stats.lastUs);
    while (last.length() < 10) {
      last = " " + last;
    }
    out += last;
    String maxUs(task.stats.maxUs);
    while (maxUs.length() < 10) {
      maxUs = " " + maxUs;
    }
    out += maxUs;
    String cpu = String(task.stats.cpuPermille / 10) + "." + String(task.stats.cpuPermille % 10);
    while (cpu.length() < 7) {
      cpu = " " + cpu;
    }
    out += cpu;
    out += '\n';
  }
  out += "\nticks=";
  out += String(ticks_);
  out += " max_tick_us=";
  out += String(maxTickUs_);
  out += " busy_permille=";
  out += String(busyPermille_);
  out += '\n';
  return out;
}

}  // namespace nhos
