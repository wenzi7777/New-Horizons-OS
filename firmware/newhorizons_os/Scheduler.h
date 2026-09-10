#pragma once

#include <Arduino.h>

namespace nhos {

// Cooperative, non-preemptive task table for the runtime loop.
//
// Deliberately NOT built on FreeRTOS tasks. Every module in this firmware
// shares state through plain members with no locking anywhere, which is only
// safe because exactly one thread ever touches them. The value this class
// adds is accounting and introspection, not concurrency: it replaces a
// hardcoded call list with a table that can be measured, named, and reported.
//
// Dispatch is in registration order, so a table registered in the order the
// old loop() called things behaves identically to it.
class Scheduler {
 public:
  // Returns false when the runtime is parked (soft-off) and only alwaysRun
  // tasks should be dispatched. Evaluated lazily, per task, so a task that
  // flips the gate mid-tick affects the tasks after it exactly as the
  // equivalent `if` in the old loop() did.
  using RuntimeGate = bool (*)();
  using TaskFn = void (*)();

  struct TaskStats {
    uint32_t invocations = 0;
    uint32_t lastUs = 0;
    uint32_t maxUs = 0;
    uint64_t totalUs = 0;
    uint32_t windowUs = 0;  // accumulated within the current CPU window
    uint16_t cpuPermille = 0;
  };

  void setRuntimeGate(RuntimeGate gate) { gate_ = gate; }

  bool registerTask(const char* name, TaskFn fn, bool alwaysRun, uint32_t periodUs = 0);
  bool setEnabled(const char* name, bool enabled);

  // Runs one pass over the table. Call once per loop() iteration.
  void tick();

  uint32_t tickCount() const { return ticks_; }
  uint32_t maxTickUs() const { return maxTickUs_; }
  // Share of wall time spent inside the dispatch loop, over the last window.
  //
  // Deliberately NOT a deadline alarm. An earlier version compared each
  // tick against the scan interval and called the excess an "overrun", which
  // fired on 4083 of 4085 ticks on real hardware -- a tick that performs a
  // scan legitimately consumes most of a frame period, so the counter only
  // ever restated "the device is busy". ScanHealth::overrunFrames is the
  // authority on missed sampling deadlines; this reports load, and the
  // per-task figures below attribute it.
  uint16_t busyPermille() const { return busyPermille_; }
  uint8_t taskCount() const { return count_; }

  String statusJson() const;
  // Column-aligned `ps`-style dump for /proc/tasks.
  String tasksText() const;

 private:
  static constexpr uint8_t kMaxTasks = 24;
  // Rolling window over which per-task CPU share is computed.
  static constexpr uint32_t kCpuWindowUs = 1000000;

  struct Task {
    const char* name = nullptr;
    TaskFn fn = nullptr;
    uint32_t periodUs = 0;
    uint32_t nextDueUs = 0;
    bool alwaysRun = false;
    bool enabled = true;
    TaskStats stats;
  };

  int8_t indexOf(const char* name) const;
  void closeCpuWindowIfDue(uint32_t nowUs);

  Task tasks_[kMaxTasks];
  uint8_t count_ = 0;
  RuntimeGate gate_ = nullptr;
  uint32_t ticks_ = 0;
  uint32_t maxTickUs_ = 0;
  uint16_t busyPermille_ = 0;
  uint32_t windowBusyUs_ = 0;
  uint32_t windowStartedUs_ = 0;
};

}  // namespace nhos
