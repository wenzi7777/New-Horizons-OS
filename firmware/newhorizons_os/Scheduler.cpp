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

bool Scheduler::setUrgentTask(const char* name, DueFn due) {
  const int8_t index = indexOf(name);
  if (index < 0 || due == nullptr) {
    return false;
  }
  urgent_ = index;
  urgentDue_ = due;
  return true;
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
    if (!dispatch(i)) {
      continue;
    }
    if (urgent_ >= 0 && i != static_cast<uint8_t>(urgent_) && urgentDue_() &&
        dispatch(static_cast<uint8_t>(urgent_))) {
      ++urgentRuns_;
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

bool Scheduler::dispatch(uint8_t index) {
  Task& task = tasks_[index];
  if (!task.enabled) {
    return false;
  }
  // Lazily, not once per tick: the gate depends on state that earlier
  // tasks in this same pass may have just changed.
  if (!task.alwaysRun && gate_ != nullptr && !gate_()) {
    return false;
  }
  const uint32_t nowUs = micros();
  if (task.periodUs != 0 && static_cast<int32_t>(nowUs - task.nextDueUs) < 0) {
    return false;
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
  return true;
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
  // One snprintf per task into a buffer reserved up front: the String-per-
  // field version cost ~2.3 ms of every `status`, almost all of it in heap
  // churn. Task names are fixed identifiers, so they need no escaping.
  String json;
  json.reserve(160 + count_ * 150);
  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"ticks\":%lu,\"max_tick_us\":%lu,\"busy_permille\":%u,\"task_count\":%u,\"urgent_runs\":%lu,\"tasks\":[",
           static_cast<unsigned long>(ticks_), static_cast<unsigned long>(maxTickUs_),
           static_cast<unsigned>(busyPermille_), static_cast<unsigned>(count_),
           static_cast<unsigned long>(urgentRuns_));
  json += buf;
  for (uint8_t i = 0; i < count_; ++i) {
    const Task& task = tasks_[i];
    snprintf(buf, sizeof(buf),
             "%s{\"name\":\"%s\",\"enabled\":%s,\"always_run\":%s,\"period_us\":%lu,\"invocations\":%lu,"
             "\"last_us\":%lu,\"max_us\":%lu,\"cpu_permille\":%u}",
             i != 0 ? "," : "", task.name, task.enabled ? "true" : "false", task.alwaysRun ? "true" : "false",
             static_cast<unsigned long>(task.periodUs), static_cast<unsigned long>(task.stats.invocations),
             static_cast<unsigned long>(task.stats.lastUs), static_cast<unsigned long>(task.stats.maxUs),
             static_cast<unsigned>(task.stats.cpuPermille));
    json += buf;
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
