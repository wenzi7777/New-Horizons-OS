// ControlServer command handlers: status and diagnostics, clock, services, logs, maintenance, OTA, reboot.
//
// One member per command, registered in ControlCommandTable.cpp. A handler
// never checks maintenance mode itself: the table's kCmdNeedsMaintenance
// flag is enforced once, in processCommand(), before it runs.
#include "ControlServer.h"
#include "EspNowOtaReceiver.h"
#include <esp_timer.h>
#include <utility>
#include <vector>
#include "Config.h"
#include "BatteryStatusJsonMerge.h"
#include "ActionButtonPolicy.h"
#include "EspNowPairing.h"  // kEspNowPairingFailFlagKey, so this stays in sync with EspNowPairing.cpp's own check
#include "EspNowTimeWire.h"
#include "JsonUtils.h"
#include "ReplyPacing.h"
#include "UpdateChannel.h"
#include "VersionCompare.h"

namespace nhos {

// status, query
String ControlServer::cmdStatus(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const ScanHealth health = scanner_->health();
  const String uid = deviceUidString();
  String scanTiming = "{";
  scanTiming.reserve(64);
  bool scanTimingFirst = true;
  jsonUnsignedField(scanTiming, "target_fps", health.targetFps, scanTimingFirst);
  jsonUnsignedField(scanTiming, "settle_us", health.settleUs, scanTimingFirst);
  jsonUnsignedField(scanTiming, "send_every_n_frames", health.sendEveryNFrames, scanTimingFirst);
  scanTiming += "}";
  const String streamBuffer = deviceConfig_ ? deviceConfig_->streamBufferJson() : String("{\"enabled\":false,\"mode\":\"standard\",\"depth_frames\":0}");

  String runtime = "{";
  runtime.reserve(160);
  bool runtimeFirst = true;
  jsonRawField(runtime, "scan_timing", scanTiming, runtimeFirst);
  jsonRawField(runtime, "stream_buffer", streamBuffer, runtimeFirst);
  jsonStringField(runtime, "protocol", kProtocolName, runtimeFirst);
  jsonStringField(runtime, "mode", boot_->modeName(), runtimeFirst);
  runtime += "}";

  // The same full status goes out on every transport. Over ESP-NOW it is
  // bigger than one control frame (~13 KB against 7680 B), so it travels
  // as a paged reply (EspNowResultPager.h), just as UDP splits it into
  // result_chunk datagrams.
  String data = "{";
  data.reserve(1536);
  bool first = true;
  jsonStringField(data, "device_uid", uid, first);
  jsonStringField(data, "device_name", String("NHOS-") + uid, first);
  jsonStringField(data, "protocol", kProtocolName, first);
  jsonStringField(data, "mode", boot_->modeName(), first);
  jsonStringField(data, "firmware_version", kFirmwareVersion, first);
  jsonBoolField(data, "firmware_prerelease", isPrereleaseVersion(kFirmwareVersion), first);
  jsonStringField(data, "hardware_model", kHardwareModel, first);
  jsonRawField(data, "matrix_shape", scanner_->matrixShapeJson(), first);
  jsonRawField(data, "matrix_layout", scanner_->matrixLayoutJson(), first);
  jsonRawField(data, "runtime", runtime, first);
  jsonRawField(data, "wifi", wifi_->statusJson(), first);
  jsonRawField(data, "battery", batteryStatusJson(), first);
  jsonRawField(data, "battery_gauge", batteryGauge_ ? batteryGauge_->statusJson() : "{}", first);
  jsonRawField(data, "power", powerState_ ? powerState_->statusJson() : "{}", first);
  jsonRawField(data, "config", deviceConfig_ ? deviceConfig_->statusJson() : "{}", first);
  jsonRawField(data, "logging", storage_ ? storage_->logStatusJson() : "{}", first);
  jsonRawField(data, "ota", deviceConfig_ ? deviceConfig_->otaJson() : "{}", first);
  // In Direct mode the HTTP OtaManager is never used (no WiFi), so its
  // state would always read "idle" and hide a relay that is actually
  // running -- report the ESP-NOW relay's own progress instead.
  jsonRawField(data, "update_state",
               espNowOta_ != nullptr ? espNowOta_->statusJson()
                                      : (ota_ ? ota_->lastStatusJson() : String("{}")),
               first);
  jsonRawField(data, "ota_rollback", boot_ ? boot_->otaRollbackStatusJson() : "{}", first);
  jsonRawField(data, "faults", faults_ ? faults_->statusJson() : "{}", first);
  jsonRawField(data, "scheduler", scheduler_ ? scheduler_->statusJson() : "{}", first);
  jsonRawField(data, "airtime", arbiter_ ? arbiter_->statusJson() : "{}", first);
  jsonRawField(data, "services", services_ ? services_->statusJson() : "{}", first);
  jsonRawField(data, "clock", clock_ ? clock_->statusJson() : "{}", first);
  jsonRawField(data, "power_governor", governor_ ? governor_->statusJson() : "{}", first);
  jsonRawField(data, "apps", apps_ ? apps_->statusJson() : "{}", first);
  jsonRawField(data, "filter", deviceConfig_ ? deviceConfig_->filterJson() : "{}", first);
  jsonBoolField(data, "stream_raw_adc", deviceConfig_ ? deviceConfig_->data().streamRawAdc : false, first);
  jsonRawField(data, "imu", imu_ ? imu_->statusJson() : "{}", first);
  jsonRawField(data, "magnetometer", magnetometer_ ? magnetometer_->statusJson() : "{}", first);
  jsonRawField(data, "stream_buffer", streamBuffer, first);
  jsonRawField(data, "calibration", calibration_ ? calibration_->statusJson(maintenanceMode()) : "{}", first);
  jsonRawField(data, "indicators", indicatorsStatusJson(), first);
  jsonRawField(data, "action_button", actionButtonStatusJson(), first);
  jsonRawField(data, "scan_health", scanner_->healthJson(), first);
  jsonRawField(data, "findme", findme_ ? findme_->statusJson() : "{}", first);
  data += "}";
  return ok(cmd, "status", data);
}

// sensor_sample
String ControlServer::cmdSensorSample(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  // The latest readings, for a readout to chart: the same numbers the flow
  // ops imu()/mag()/battery() see. Read-only, and small enough to poll.
  String data = "{\"imu\":{";
  float imuSample[kImuSampleFloats] = {0};
  const bool imuValid = imu_ != nullptr && imu_->copyLatestSample(imuSample);
  data += "\"valid\":";
  data += imuValid ? "true" : "false";
  if (imuValid) {
    static const char* const kImuKeys[] = {"ax", "ay", "az", "gx", "gy", "gz"};
    for (uint8_t i = 0; i < 6; ++i) {
      data += ",\"";
      data += kImuKeys[i];
      data += "\":";
      data += String(imuSample[i], 4);
    }
  }
  data += "},\"mag\":{";
  float magSample[3] = {0};
  const bool magValid = magnetometer_ != nullptr && magnetometer_->copyLatestSample(magSample);
  data += "\"valid\":";
  data += magValid ? "true" : "false";
  if (magValid) {
    static const char* const kMagKeys[] = {"mx", "my", "mz"};
    for (uint8_t i = 0; i < 3; ++i) {
      data += ",\"";
      data += kMagKeys[i];
      data += "\":";
      data += String(magSample[i], 3);
    }
  }
  data += "},\"battery\":{";
  BatteryGaugeSample gauge;
  const bool gaugeValid = batteryGauge_ != nullptr && batteryGauge_->copyLatestSample(gauge);
  data += "\"valid\":";
  data += gaugeValid ? "true" : "false";
  if (gaugeValid) {
    data += ",\"percent\":";
    data += String(static_cast<float>(gauge.socCentiPercent) / 100.0f, 2);
    data += ",\"mv\":";
    data += String(gauge.vbatMv);
  }
  data += "}}";
  return ok(cmd, "sensor_sample", data);
}

// memory_status
String ControlServer::cmdMemoryStatus(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  // Diagnostic JSON fields: "heap_total", "heap_used".
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
  data += ",\"heap_min_free\":";
  data += ESP.getMinFreeHeap();
  data += "}";
  return ok(cmd, "memory_status", data);
}

// task_list
String ControlServer::cmdTaskList(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!scheduler_) {
    return error(cmd, "scheduler_unavailable");
  }
  return ok(cmd, "task_list", scheduler_->statusJson());
}

// clock_sync
String ControlServer::cmdClockSync(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!clock_) {
    return error(cmd, "clock_unavailable");
  }
  // Only starts the exchange: the result lands in status.clock a moment
  // later, so the command channel is never held for a network round trip.
  if (!clock_->requestSync()) {
    return error(cmd, "clock_source_unavailable");
  }
  return ok(cmd, "clock_sync_started", clockStatusData());
}

// clock_status
String ControlServer::cmdClockStatus(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  // A few hundred bytes, unlike `status`: cheap enough to poll after a
  // clock_sync, even through a Hub.
  if (!clock_) {
    return error(cmd, "clock_unavailable");
  }
  return ok(cmd, "clock_status", clockStatusData());
}

// service_list
String ControlServer::cmdServiceList(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!services_) {
    return error(cmd, "service_manager_unavailable");
  }
  return ok(cmd, "service_list", services_->statusJson());
}

// service_restart
String ControlServer::cmdServiceRestart(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!services_) {
    return error(cmd, "service_manager_unavailable");
  }
  const String name = extractString(request, "name");
  if (name.isEmpty()) {
    return error(cmd, "name_required");
  }
  if (!services_->exists(name.c_str())) {
    return error(cmd, "unknown_service");
  }
  // Reports the post-restart state either way: a restart that fails still
  // leaves the service supervised and retrying, which is worth seeing.
  const bool running = services_->restart(name.c_str());
  String data = "{\"name\":\"";
  data += jsonEscape(name);
  data += "\",\"running\":";
  data += running ? "true" : "false";
  data += ",\"services\":";
  data += services_->statusJson();
  data += "}";
  return ok(cmd, running ? "service_restarted" : "service_restart_failed", data);
}

// scan_health
String ControlServer::cmdScanHealth(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  return ok(cmd, "scan_health", scanner_->healthJson());
}

// storage_status
String ControlServer::cmdStorageStatus(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  return ok(cmd, "storage_status", storage_->storageStatusJson());
}

// enter_maintenance
String ControlServer::cmdEnterMaintenance(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  scanner_->stop();
  boot_->enterMaintenance(false);
  return ok(cmd, "maintenance_entered");
}

// exit_maintenance
String ControlServer::cmdExitMaintenance(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  boot_->exitMaintenance();
  scanner_->start();
  return ok(cmd, "maintenance_exited");
}

// findme_discover
String ControlServer::cmdFindmeDiscover(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (findme_) {
    findme_->discoverNow();
    return ok(cmd, "findme_discovery_started", findme_->statusJson());
  }
  return error(cmd, "findme_unavailable");
}

// findme_switch_gateway
String ControlServer::cmdFindmeSwitchGateway(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (findme_) {
    findme_->switchGateway(
        extractString(request, "preferred_gateway_id"),
        extractString(request, "claim_id"),
        static_cast<uint32_t>(extractInt(request, "ttl_ms", 30000)));
    return ok(cmd, "findme_switch_started", findme_->statusJson());
  }
  return error(cmd, "findme_unavailable");
}

// power_set_state
String ControlServer::cmdPowerSetState(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!powerState_) {
    return error(cmd, "power_state_unavailable");
  }
  const String nextState = extractString(request, "state");
  if (!powerState_->requestStateByName(nextState, power_ && power_->chargerDetected())) {
    return error(cmd, "power_state_invalid");
  }
  String data = "{\"power\":";
  data += powerState_->statusJson();
  data += "}";
  return ok(cmd, "power_state_updated", data);
}

// battery_status
String ControlServer::cmdBatteryStatus(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  // The battery card's own slice of `status`: what the Desktop polls while
  // a gauge resync runs. A full status costs ~14 ms of main-loop time to
  // build (it is ~18 KB); this is the ~1 KB the card actually reads.
  String data = "{";
  bool first = true;
  jsonRawField(data, "battery", batteryStatusJson(), first);
  jsonRawField(data, "battery_gauge", batteryGauge_ ? batteryGauge_->statusJson() : "{}", first);
  jsonRawField(data, "power", powerState_ ? powerState_->statusJson() : "{}", first);
  data += "}";
  return ok(cmd, "battery_status", data);
}

// resync_battery_gauge
String ControlServer::cmdResyncBatteryGauge(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!batteryProfileCommandSupported(NHOS_BOARD_HAS_MAX17048) ||
      !batteryGauge_) {
    return error(cmd, "battery_gauge_unavailable");
  }
  const GaugeResyncResult result = batteryGauge_->requestResync(millis());
  if (result == GaugeResyncResult::Failed ||
      result == GaugeResyncResult::Unavailable) {
    return error(cmd, "battery_gauge_resync_failed");
  }
  String data = "{";
  bool first = true;
  jsonRawField(data, "battery", batteryStatusJson(), first);
  jsonRawField(data, "battery_gauge", batteryGauge_->statusJson(), first);
  data += "}";
  return ok(cmd,
            result == GaugeResyncResult::InProgress
                ? "battery_gauge_resync_in_progress"
                : "battery_gauge_resync_started",
            data);
}

// log_tail
String ControlServer::cmdLogTail(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  // A zero or negative count would wrap to a huge size_t and mean "the
  // whole log".
  const int maxLines = extractInt(request, "max_lines", 50);
  return ok(cmd, "log_tail", storage_->tailLog(maxLines > 0 ? static_cast<size_t>(maxLines) : 50));
}

// dmesg
String ControlServer::cmdDmesg(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  // Serves the RAM ring, not the flash log: no SPIFFS reads, and it still
  // works when logging to flash has been switched off.
  return ok(cmd, "dmesg", storage_->kmsgJson());
}

// log_clear
String ControlServer::cmdLogClear(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  storage_->clearLog();
  return ok(cmd, "log_cleared");
}

// crash_log
String ControlServer::cmdCrashLog(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!faults_) {
    return error(cmd, "fault_recorder_unavailable");
  }
  String data = "{\"summary\":";
  data += faults_->statusJson();
  data += ",\"faults\":";
  data += faults_->historyJson();
  data += ",\"core_dump\":{\"available\":";
  data += faults_->coreDumpAvailable() ? "true" : "false";
  data += ",\"bytes\":";
  data += String(static_cast<uint32_t>(faults_->coreDumpSize()));
  // The raw ELF rides the existing file channel rather than a new command:
  // file_read_chunk --scope proc --path crash.elf, then analyse offline
  // with esp-coredump.
  data += ",\"scope\":\"proc\",\"path\":\"crash.elf\"}}";
  return ok(cmd, "crash_log", data);
}

// health
String ControlServer::cmdHealth(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!lifetime_) {
    return error(cmd, "lifetime_stats_unavailable");
  }
  return ok(cmd, "health", lifetime_->healthJson());
}

// crash_clear
String ControlServer::cmdCrashClear(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  if (!faults_) {
    return error(cmd, "fault_recorder_unavailable");
  }
  const bool erased = faults_->clear();
  return ok(cmd, erased ? "crash_log_cleared" : "crash_log_cleared_dump_retained");
}

// check_update
String ControlServer::cmdCheckUpdate(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (espNowOta_ != nullptr) {
    // Direct mode: the manifest lives behind a Hub round trip, so this
    // can only start the check -- the result lands in update_state a
    // moment later (poll `status`, which the Desktop app already does).
    const bool started = espNowOta_->requestCheck(requestedManifestUrl(request));
    String data = "{";
    bool f = true;
    jsonRawField(data, "update_state", espNowOta_->statusJson(), f);
    data += "}";
    return started ? ok(cmd, "update_check_started", data)
                   : error(cmd, "espnow_ota_busy_or_unpaired");
  }
  UpdateInfo info = ota_->checkUpdate(requestedManifestUrl(request));
  String data = "{";
  data.reserve(512);
  bool first = true;
  jsonBoolField(data, "available", info.available, first);
  jsonStringField(data, "version", info.version, first);
  jsonStringField(data, "url", info.url, first);
  jsonStringField(data, "sha256", info.sha256, first);
  jsonUnsignedField(data, "size", static_cast<unsigned long>(info.size), first);
  jsonStringField(data, "changelog_url", info.changelogUrl, first);
  jsonRawField(data, "update_state", ota_->lastStatusJson(), first);
  jsonStringField(data, "error", info.error, first);
  data += "}";
  return ok(cmd, info.error.isEmpty() ? "update_checked" : "update_check_failed", data);
}

// apply_update
String ControlServer::cmdApplyUpdate(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (espNowOta_ != nullptr) {
    const bool started = espNowOta_->requestApply(requestedManifestUrl(request));
    String data = "{";
    bool f = true;
    jsonRawField(data, "update_state", espNowOta_->statusJson(), f);
    data += "}";
    return started ? ok(cmd, "update_started", data)
                   : error(cmd, "espnow_ota_busy_or_unpaired");
  }
  otaPendingManifestUrl_ = requestedManifestUrl(request);
  otaPending_ = true;
  return ok(
      cmd,
      "update_started",
      "{\"update_state\":{\"phase\":\"downloading\",\"operation\":\"apply_update\",\"current_file\":\"firmware\",\"last_error\":\"\",\"last_result\":\"starting\",\"reboot_required\":true}}");
}

// reboot
String ControlServer::cmdReboot(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  boot_->requestReboot();
  return ok(cmd, "reboot_scheduled");
}

// reboot_wifi_setup
String ControlServer::cmdRebootWifiSetup(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  // For boards whose action button is broken: the reply goes out first,
  // then the device restarts straight into the WiFi setup portal (also out
  // of ESP-NOW mode, as the boot button does) and drops off the network.
  boot_->requestWifiSetupOnNextBoot();
  boot_->requestReboot();
  return ok(cmd, "wifi_setup_reboot_scheduled");
}

}  // namespace nhos
