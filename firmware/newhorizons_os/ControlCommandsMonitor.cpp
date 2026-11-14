// System monitor: one-shot snapshot (sysmon), FreeRTOS threads (thread_list),
// and operator control of scheduler tasks and supervised services.
//
// Every control here is runtime only -- nothing is persisted, a reboot puts
// everything back. Protection (which tasks/services may be stopped) is
// enforced by Scheduler and ServiceManager themselves, not by whichever UI
// sent the command.
#include "ControlServer.h"

#include "HeapWatermark.h"
#include "JsonUtils.h"

namespace nhos {

String ControlServer::memoryJson() const {
  const uint32_t heapTotal = ESP.getHeapSize();
  const uint32_t heapFree = ESP.getFreeHeap();
  String data = "{\"heap_free\":";
  data += heapFree;
  data += ",\"heap_total\":";
  data += heapTotal;
  data += ",\"heap_used\":";
  data += heapTotal > heapFree ? heapTotal - heapFree : 0;
  data += ",\"heap_largest_free_block\":";
  data += ESP.getMaxAllocHeap();
  data += ",\"heap_min_largest_free_block\":";
  data += HeapWatermark::minLargestFreeBlock();
  data += ",\"heap_min_free\":";
  data += ESP.getMinFreeHeap();
  data += "}";
  return data;
}

String ControlServer::appListJson(const String& outputsFor) const {
  String json = apps_ ? apps_->statusJson(outputsFor) : String("{}");
  if (appGovernor_ != nullptr && json.endsWith("}")) {
    json.remove(json.length() - 1);
    json += json.endsWith("{") ? "" : ",";
    json += "\"governor\":";
    json += appGovernor_->statusJson();
    json += "}";
  }
  return json;
}

// sysmon -- everything a system monitor draws, in one reply. Over ESP-NOW a
// Hub carries one command per device at a time, so five polls a second
// (task_list, service_list, app_list, memory_status, scan_health) became a
// queue; this is one. Large replies page through result_page as usual.
String ControlServer::cmdSysmon(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String threads = threads_.snapshotJson();
  String data;
  data.reserve(8192);
  data += "{\"uptime_ms\":";
  data += String(millis());
  data += ",\"loop_busy_permille\":";
  data += String(scheduler_ ? scheduler_->busyPermille() : 0);
  bool first = false;
  jsonRawField(data, "scheduler", scheduler_ ? scheduler_->statusJson() : "{}", first);
  jsonRawField(data, "services", services_ ? services_->statusJson() : "{}", first);
  jsonRawField(data, "apps", appListJson(), first);
  jsonRawField(data, "memory", memoryJson(), first);
  jsonRawField(data, "scan", scanner_->healthJson(), first);
  jsonRawField(data, "threads", threads, first);
  jsonRawField(data, "airtime", arbiter_ ? arbiter_->statusJson() : "{}", first);
  data += "}";
  return ok(cmd, "sysmon", data);
}

// thread_list
String ControlServer::cmdThreadList(const CommandContext& ctx) {
  return ok(ctx.cmd, "thread_list", threads_.snapshotJson());
}

// task_stop, task_start
String ControlServer::cmdTaskStopStart(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!scheduler_) {
    return error(cmd, "scheduler_unavailable");
  }
  const String name = extractString(ctx.request, "name");
  if (name.isEmpty()) {
    return error(cmd, "name_required");
  }
  const bool stopping = cmd == "task_stop";
  const Scheduler::OpResult result =
      stopping ? scheduler_->operatorStop(name.c_str()) : scheduler_->operatorStart(name.c_str());
  if (result != Scheduler::OpResult::Ok) {
    return error(cmd, Scheduler::opResultCode(result));
  }
  return ok(cmd, stopping ? "task_stopped" : "task_started", scheduler_->statusJson());
}

// task_set_period {name, period_us} or {name, period_us:"default"}
String ControlServer::cmdTaskSetPeriod(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!scheduler_) {
    return error(cmd, "scheduler_unavailable");
  }
  const String name = extractString(request, "name");
  if (name.isEmpty()) {
    return error(cmd, "name_required");
  }
  Scheduler::OpResult result;
  if (extractString(request, "period_us") == "default") {
    // Protection still applies to a reset: the guarded tasks were never moved.
    result = scheduler_->isProtected(name.c_str()) ? Scheduler::OpResult::Protected
                                                   : scheduler_->operatorResetPeriod(name.c_str());
  } else {
    const int periodUs = extractInt(request, "period_us", -1);
    if (periodUs < 0) {
      return error(cmd, "period_required");
    }
    result = scheduler_->operatorSetPeriod(name.c_str(), static_cast<uint32_t>(periodUs));
  }
  if (result != Scheduler::OpResult::Ok) {
    return error(cmd, Scheduler::opResultCode(result));
  }
  return ok(cmd, "task_period_set", scheduler_->statusJson());
}

// service_stop, service_start
String ControlServer::cmdServiceStopStart(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!services_) {
    return error(cmd, "service_manager_unavailable");
  }
  const String name = extractString(ctx.request, "name");
  if (name.isEmpty()) {
    return error(cmd, "name_required");
  }
  const bool stopping = cmd == "service_stop";
  const ServiceManager::OpResult result =
      stopping ? services_->stop(name.c_str()) : services_->start(name.c_str());
  if (result != ServiceManager::OpResult::Ok) {
    return error(cmd, ServiceManager::opResultCode(result));
  }
  return ok(cmd, stopping ? "service_stopped" : "service_started", services_->statusJson());
}

}  // namespace nhos
