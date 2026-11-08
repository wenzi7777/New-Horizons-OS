#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace nhos {

// The FreeRTOS threads underneath the cooperative Scheduler: loopTask (which
// runs the whole Scheduler table), WiFi/lwIP, esp_timer, ipc and the two IDLE
// tasks. Read-only by design -- these belong to the core and the SDK, and
// suspending one of them is how a device stops answering. The Scheduler's own
// tasks are where operator control lives (task_stop and friends).
//
// CPU figures are deltas between two calls of snapshotJson(), from the
// run-time stats counter the core already maintains
// (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS, esp_timer based, 32-bit
// microseconds -- unsigned subtraction survives its ~71 minute wrap). The
// first call after boot has nothing to compare with and reports them as null
// rather than inventing a figure.
class ThreadMonitor {
 public:
  static constexpr uint8_t kMaxThreads = 32;

  // {"wall_us","cores":[{"core","busy_permille"}],"thread_count","threads":[
  //   {"name","state","priority","base_priority","core","stack_free_min",
  //    "cpu_permille"}]}
  // `core` is the affinity (-1: either core); cpu_permille is a share of the
  // whole chip (both cores), so the column sums to about 1000 like a desktop
  // task manager's.
  String snapshotJson();

  static const char* stateName(eTaskState state);

 private:
  struct Previous {
    TaskHandle_t handle = nullptr;
    uint32_t runTime = 0;
  };

  TaskStatus_t status_[kMaxThreads];
  Previous previous_[kMaxThreads];
  uint8_t previousCount_ = 0;
  uint32_t previousWall_ = 0;
  bool hasPrevious_ = false;
};

}  // namespace nhos
