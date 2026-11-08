#include "ThreadMonitor.h"

#include <string.h>

#include "JsonUtils.h"

namespace nhos {

const char* ThreadMonitor::stateName(eTaskState state) {
  switch (state) {
    case eRunning: return "running";
    case eReady: return "ready";
    case eBlocked: return "blocked";
    case eSuspended: return "suspended";
    case eDeleted: return "deleted";
    default: return "unknown";
  }
}

String ThreadMonitor::snapshotJson() {
  uint32_t wall = 0;
  const UBaseType_t count = uxTaskGetSystemState(status_, kMaxThreads, &wall);
  const uint32_t wallDelta = wall - previousWall_;
  const bool haveDelta = hasPrevious_ && wallDelta != 0;
  const uint32_t cores = configNUMBER_OF_CORES;

  // Per-core busy share from that core's IDLE task. Unknown until a delta.
  int32_t coreBusy[configNUMBER_OF_CORES];
  for (uint32_t c = 0; c < cores; ++c) {
    coreBusy[c] = -1;
  }

  String threads;
  threads.reserve(count * 150);
  Previous next[kMaxThreads];
  char buf[200];
  for (UBaseType_t i = 0; i < count; ++i) {
    const TaskStatus_t& task = status_[i];
    const uint32_t runTime = static_cast<uint32_t>(task.ulRunTimeCounter);
    next[i].handle = task.xHandle;
    next[i].runTime = runTime;

    int32_t cpuPermille = -1;
    if (haveDelta) {
      for (uint8_t p = 0; p < previousCount_; ++p) {
        if (previous_[p].handle == task.xHandle) {
          const uint32_t ran = runTime - previous_[p].runTime;
          cpuPermille = static_cast<int32_t>((static_cast<uint64_t>(ran) * 1000ULL) /
                                             (static_cast<uint64_t>(wallDelta) * cores));
          // IDLE<n> is pinned to core n; busy = 1 - idle, per core.
          if (strncmp(task.pcTaskName, "IDLE", 4) == 0) {
            const int core = atoi(task.pcTaskName + 4);
            if (core >= 0 && static_cast<uint32_t>(core) < cores) {
              const uint32_t idlePermille = static_cast<uint32_t>(
                  (static_cast<uint64_t>(ran) * 1000ULL) / wallDelta);
              coreBusy[core] = idlePermille >= 1000 ? 0 : static_cast<int32_t>(1000 - idlePermille);
            }
          }
          break;
        }
      }
    }
    const int core = task.xCoreID == tskNO_AFFINITY ? -1 : static_cast<int>(task.xCoreID);
    char cpu[12];
    if (cpuPermille < 0) {
      strcpy(cpu, "null");
    } else {
      snprintf(cpu, sizeof(cpu), "%ld", static_cast<long>(cpuPermille));
    }
    // Thread names come from the SDK and other libraries, so escape them.
    snprintf(buf, sizeof(buf),
             "%s{\"name\":\"%s\",\"state\":\"%s\",\"priority\":%u,\"base_priority\":%u,\"core\":%d,"
             "\"stack_free_min\":%lu,\"cpu_permille\":%s}",
             i != 0 ? "," : "", jsonEscape(String(task.pcTaskName)).c_str(),
             stateName(task.eCurrentState), static_cast<unsigned>(task.uxCurrentPriority),
             static_cast<unsigned>(task.uxBasePriority), core,
             static_cast<unsigned long>(task.usStackHighWaterMark), cpu);
    threads += buf;
  }

  for (UBaseType_t i = 0; i < count; ++i) {
    previous_[i] = next[i];
  }
  previousCount_ = static_cast<uint8_t>(count);
  previousWall_ = wall;
  hasPrevious_ = true;

  String json;
  json.reserve(threads.length() + 160);
  json += "{\"wall_us\":";
  json += String(haveDelta ? wallDelta : 0);
  json += ",\"cores\":[";
  for (uint32_t c = 0; c < cores; ++c) {
    if (c != 0) {
      json += ",";
    }
    json += "{\"core\":";
    json += String(c);
    json += ",\"busy_permille\":";
    json += coreBusy[c] < 0 ? String("null") : String(coreBusy[c]);
    json += "}";
  }
  // uxTaskGetSystemState fills nothing when the array is too small, so an
  // empty list next to a larger total means "more than kMaxThreads".
  json += "],\"thread_count\":";
  json += String(static_cast<unsigned>(uxTaskGetNumberOfTasks()));
  json += ",\"threads\":[";
  json += threads;
  json += "]}";
  return json;
}

}  // namespace nhos
