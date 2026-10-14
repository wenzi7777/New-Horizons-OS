// The device's command table: every command ControlServer accepts, its
// handler, and the flags the dispatcher and the Desktop act on. The order is
// the order `commands` reports. See CommandRegistry.h.
#include "ControlServer.h"

#include "JsonUtils.h"

namespace nhos {

const CommandSpec* ControlServer::commandTable(size_t& count) {
  static const CommandSpec kCommands[] = {
      {"status", &ControlServer::cmdStatus, kCmdHeavy | kCmdStatusSnapshot,
       "Query device status."},
      {"query", &ControlServer::cmdStatus, kCmdHeavy | kCmdStatusSnapshot,
       "Alias of status."},
      {"sensor_sample", &ControlServer::cmdSensorSample, 0,
       "Read the latest IMU, magnetometer and battery values (firmware v1.6.0+)."},
      {"memory_status", &ControlServer::cmdMemoryStatus, kCmdStatusSnapshot,
       "Heap totals and fragmentation."},
      {"task_list", &ControlServer::cmdTaskList, 0,
       "List the scheduler's task table with per-task timing and CPU share."},
      {"app_list", &ControlServer::cmdAppList, 0,
       "List installed on-device apps with state, time budget and measured cost."},
      {"app_view", &ControlServer::cmdAppView, kCmdQuietLed,
       "What each app slot shows on the OLED, status LED and external strip."},
      {"app_enable", &ControlServer::cmdAppEnable, 0,
       "Enable an installed app slot."},
      {"app_disable", &ControlServer::cmdAppEnable, 0,
       "Disable an app slot without uninstalling its package."},
      {"app_revive", &ControlServer::cmdAppRevive, 0,
       "Clear a budget-overrun kill and restart the app."},
      {"app_events", &ControlServer::cmdAppEvents, kCmdQuietLed,
       "Events emitted by apps since a sequence number, with a dropped count."},
      {"app_load_flow", &ControlServer::cmdAppLoadFlow, 0,
       "Load a flow graph into slot 0; rejected at load time if it exceeds the budget."},
      {"app_unload_flow", &ControlServer::cmdAppUnloadFlow, 0,
       "Unload the flow graph in slot 0."},
      {"app_list_packages", &ControlServer::cmdAppListPackages, 0,
       "Installed app packages and which slot each is bound to."},
      {"app_install", &ControlServer::cmdAppInstall, kCmdNeedsMaintenance,
       "Install an uploaded .nha package into the registry."},
      {"app_uninstall", &ControlServer::cmdAppUninstall, kCmdNeedsMaintenance,
       "Remove a package from the registry and delete its file."},
      {"app_activate", &ControlServer::cmdAppActivate, kCmdNeedsMaintenance,
       "Bind an installed package to a flow slot."},
      {"app_deactivate", &ControlServer::cmdAppDeactivate, kCmdNeedsMaintenance,
       "Unbind a package from its slot, leaving it installed."},
      {"app_verify", &ControlServer::cmdAppVerify, 0,
       "Re-hash an installed package and compare it with the index."},
      {"app_reindex", &ControlServer::cmdAppReindex, kCmdNeedsMaintenance,
       "Rebuild the package index from the .nha files present."},
      {"config_schema", &ControlServer::cmdConfigSchema, 0,
       "Describe every configuration knob: path, type, range, and owning command."},
      {"config_get", &ControlServer::cmdConfigGet, 0,
       "Read one configuration path, or all of them when no path is given."},
      {"config_set", &ControlServer::cmdConfigSet, 0,
       "Write one configuration path; dispatches to the owning set_* handler."},
      {"capabilities", &ControlServer::cmdCapabilities, 0,
       "Report protocol, config schema hash, board features, and available subsystems."},
      {"commands", &ControlServer::cmdCommands, kCmdHeavy,
       "Every command this firmware accepts, with its flags (this table)."},
      {"set_power_profile", &ControlServer::cmdSetPowerProfile, 0,
       "Choose the idle CPU frequency policy (never applied while scanning)."},
      {"set_time", &ControlServer::cmdSetTime, 0,
       "Set a device's clock to the given time (--force overrides an active NTP/Hub source)."},
      {"clock_sync", &ControlServer::cmdClockSync, 0,
       "Re-sync the device clock now from its own source (the Gateway, else NTP, in WiFi mode; the Hub in ESP-NOW mode)."},
      {"clock_status", &ControlServer::cmdClockStatus, 0,
       "Show the device clock: source, sync age, uncertainty and its clock settings."},
      {"set_clock_config", &ControlServer::cmdSetClockConfig, 0,
       "Set the NTP server and the NTP / Gateway sync intervals."},
      {"service_list", &ControlServer::cmdServiceList, 0,
       "List supervised services with state, restart count, and health failures."},
      {"service_restart", &ControlServer::cmdServiceRestart, 0,
       "Restart one supervised service without rebooting the device."},
      {"scan_health", &ControlServer::cmdScanHealth, kCmdStatusSnapshot,
       "Query lightweight scan, stream, memory, and control health."},
      {"storage_status", &ControlServer::cmdStorageStatus, kCmdHeavy | kCmdStatusSnapshot,
       "Flash filesystem usage per category (walks SPIFFS; slow)."},
      {"enter_maintenance", &ControlServer::cmdEnterMaintenance, 0,
       "Enter maintenance mode and stop normal scanning."},
      {"exit_maintenance", &ControlServer::cmdExitMaintenance, kCmdNeedsMaintenance,
       "Exit maintenance mode."},
      {"set_scan_timing", &ControlServer::cmdSetScanTiming, 0,
       "Configure matrix scan timing. WebUI offers 60/90 FPS presets; terminal can set custom values."},
      {"set_transport", &ControlServer::cmdSetTransport, kCmdInternal,
       "Switch the stream transport between WiFi/UDP and ESP-NOW (Hub)."},
      {"set_stream_buffer", &ControlServer::cmdSetStreamBuffer, 0,
       "Enable or disable the packet ring buffer and choose standard or extended depth."},
      {"set_matrix_layout", &ControlServer::cmdSetMatrixLayout, 0,
       "Configure active analog and select pins."},
      {"set_wifi", &ControlServer::cmdSetWifi, kCmdInternal,
       "Store WiFi credentials and reconnect."},
      {"findme_discover", &ControlServer::cmdFindmeDiscover, 0,
       "Run FindMe and attach to a New Horizons Gateway on the local network."},
      {"findme_switch_gateway", &ControlServer::cmdFindmeSwitchGateway, 0,
       "Ask the device to rerun FindMe with a preferred gateway claim."},
      {"set_charge_profile", &ControlServer::cmdSetChargeProfile, 0,
       "Configure the charging profile. Profiles: ultra_slow (100mA), slow (200mA), balanced (250mA), fast (300mA), extreme (350mA)."},
      {"power_set_state", &ControlServer::cmdPowerSetState, 0,
       "Switch the device between normal mode and soft-off power states. Some boards use a remote-only soft-off UX."},
      {"set_action_button", &ControlServer::cmdSetActionButton, 0,
       "Assign the action button's short and long press actions."},
      {"set_filter", &ControlServer::cmdSetFilter, 0,
       "Configure the per-cell median and low-pass filter."},
      {"set_raw_adc", &ControlServer::cmdSetRawAdc, 0,
       "Include the raw ADC block in streamed frames."},
      {"set_battery_profile", &ControlServer::cmdSetBatteryProfile, 0,
       "Set the manual battery profile (capacity, max charge current)."},
      {"battery_status", &ControlServer::cmdBatteryStatus, 0,
       "The battery card's slice of status: charger, gauge and power state."},
      {"resync_battery_gauge", &ControlServer::cmdResyncBatteryGauge, 0,
       "Restart the fuel gauge's state-of-charge estimate."},
      {"set_imu", &ControlServer::cmdSetImu, 0,
       "Enable or disable the optional IMU service."},
      {"set_log", &ControlServer::cmdSetLog, 0,
       "Configure device rolling log level and capacity."},
      {"set_indicators", &ControlServer::cmdSetIndicators, 0,
       "Configure supported indicator hardware. Brightness expects decimal values such as 0.10, 0.35, 0.50, or 1.00."},
      {"calibration_status", &ControlServer::cmdCalibrationStatus, 0,
       "Query manual calibration status, coverage, and metadata."},
      {"calibration_enable", &ControlServer::cmdCalibrationEnable, 0,
       "Enable the saved manual calibration profile."},
      {"calibration_disable", &ControlServer::cmdCalibrationDisable, 0,
       "Disable the saved manual calibration profile."},
      {"calibration_session_begin", &ControlServer::cmdCalibrationSessionBegin, kCmdNeedsMaintenance,
       "Start a draft calibration session."},
      {"calibration_session_abort", &ControlServer::cmdCalibrationSessionAbort, kCmdNeedsMaintenance,
       "Abort the current draft calibration session."},
      {"calibration_session_commit", &ControlServer::cmdCalibrationSessionCommit, kCmdNeedsMaintenance,
       "Commit the current draft calibration session."},
      {"calibration_clear_profile", &ControlServer::cmdCalibrationClearProfile, kCmdNeedsMaintenance,
       "Delete the saved calibration profile and all stored levels."},
      {"calibration_dump_tare", &ControlServer::cmdCalibrationDumpTare, 0,
       "Dump the saved and draft tare baseline matrix cells."},
      {"calibration_dump_level", &ControlServer::cmdCalibrationDumpLevel, 0,
       "Dump one calibration level with saved and draft matrix cells."},
      {"calibration_dump_fit", &ControlServer::cmdCalibrationDumpFit, 0,
       "Dump every sensor's fitted coefficients (p = a + b*sqrt(x) + c*x) and fit result."},
      {"calibration_set_fit", &ControlServer::cmdCalibrationSetFit, kCmdNeedsMaintenance,
       "Set the fit inputs: lowest level used, each ADC's readout curve (dV = c1*x + c2*x^2) and clip level."},
      {"calibration_set_reference", &ControlServer::cmdCalibrationSetReference, kCmdNeedsMaintenance,
       "Set the pressure actually applied for one captured level (what the fit uses)."},
      {"calibration_delete_level", &ControlServer::cmdCalibrationDeleteLevel, kCmdNeedsMaintenance,
       "Delete one calibration level from the saved profile or current draft session."},
      {"calibration_capture_tare", &ControlServer::cmdCalibrationCaptureTare, kCmdNeedsMaintenance,
       "Capture the no-load tare baseline into the current draft calibration session."},
      {"calibration_tare_capture", &ControlServer::cmdCalibrationTareCapture, 0,
       "Capture a tare in normal mode (pauses the scan briefly)."},
      {"calibration_tare_clear", &ControlServer::cmdCalibrationTareClear, 0,
       "Clear the captured tare."},
      {"calibration_capture_cell", &ControlServer::cmdCalibrationCaptureCell, kCmdNeedsMaintenance,
       "Capture one sensor into the current draft calibration session."},
      {"calibration_capture_all", &ControlServer::cmdCalibrationCaptureAll, kCmdNeedsMaintenance,
       "Capture all active sensors into the current draft calibration session."},
      {"file_list", &ControlServer::cmdFileList, 0,
       "List maintenance files. Use --scope proc for live kernel state."},
      {"file_read_begin", &ControlServer::cmdFileReadBegin, 0,
       "Begin a maintenance-mode file read."},
      {"file_read_chunk", &ControlServer::cmdFileReadChunk, 0,
       "Read a maintenance-mode file chunk. The proc scope is readable outside maintenance."},
      {"file_write_begin", &ControlServer::cmdFileWriteBegin, kCmdNeedsMaintenance,
       "Begin a maintenance-mode file write."},
      {"file_write_chunk", &ControlServer::cmdFileWriteChunk, kCmdNeedsMaintenance,
       "Write a maintenance-mode file chunk."},
      {"file_write_finish", &ControlServer::cmdFileWriteFinish, kCmdNeedsMaintenance,
       "Finish a maintenance-mode file write."},
      {"file_delete", &ControlServer::cmdFileDelete, kCmdNeedsMaintenance,
       "Delete a maintenance file."},
      {"log_tail", &ControlServer::cmdLogTail, 0,
       "Read the last N log lines."},
      {"dmesg", &ControlServer::cmdDmesg, 0,
       "Tail the in-RAM kernel log ring (no flash reads)."},
      {"log_clear", &ControlServer::cmdLogClear, 0,
       "Clear device.log and device.log.1."},
      {"crash_log", &ControlServer::cmdCrashLog, 0,
       "Show recorded panics/watchdog resets, with the faulting task and PC."},
      {"health", &ControlServer::cmdHealth, 0,
       "Show device health: power-on hours, reset counts, OTA history, temperature."},
      {"crash_clear", &ControlServer::cmdCrashClear, 0,
       "Erase the recorded crash ring and the stored core dump."},
      {"set_ota_config", &ControlServer::cmdSetOtaConfig, 0,
       "Configure firmware updates: boot-time auto update, update channel (stable / beta), optional custom manifest URL."},
      {"check_update", &ControlServer::cmdCheckUpdate, 0,
       "Check the Arduino firmware update manifest."},
      {"apply_update", &ControlServer::cmdApplyUpdate, 0,
       "Apply an Arduino whole-firmware OTA update."},
      {"reboot", &ControlServer::cmdReboot, 0,
       "Reboot the current system."},
      {"reboot_wifi_setup", &ControlServer::cmdRebootWifiSetup, 0,
       "Reboot into the Wi-Fi setup portal, as holding the action button at power-on does."},
  };
  count = sizeof(kCommands) / sizeof(kCommands[0]);
  return kCommands;
}

const CommandSpec* ControlServer::findCommand(const String& name) {
  size_t count = 0;
  const CommandSpec* table = commandTable(count);
  for (size_t i = 0; i < count; ++i) {
    if (name == table[i].name) {
      return &table[i];
    }
  }
  return nullptr;
}

uint32_t ControlServer::commandSchemaHash() {
  // FNV-1a over every name and its flags, like ConfigRegistry::schemaHash():
  // a client caches the `commands` reply and refetches only when this moves.
  uint32_t hash = 2166136261u;
  auto mix = [&hash](uint8_t byte) {
    hash ^= byte;
    hash *= 16777619u;
  };
  size_t count = 0;
  const CommandSpec* table = commandTable(count);
  for (size_t i = 0; i < count; ++i) {
    for (const char* p = table[i].name; *p; ++p) {
      mix(static_cast<uint8_t>(*p));
    }
    mix(static_cast<uint8_t>(table[i].flags & 0xff));
    mix(static_cast<uint8_t>(table[i].flags >> 8));
    mix(0);
  }
  return hash;
}

String ControlServer::commandsJson() {
  size_t count = 0;
  const CommandSpec* table = commandTable(count);
  String out;
  out.reserve(count * 96);
  out += "{\"hash\":";
  out += String(commandSchemaHash());
  out += ",\"count\":";
  out += String(static_cast<unsigned>(count));
  out += ",\"commands\":[";
  for (size_t i = 0; i < count; ++i) {
    const uint16_t f = table[i].flags;
    if (i > 0) out += ",";
    out += "{\"name\":\"";
    out += table[i].name;
    out += "\",\"maintenance\":";
    out += (f & kCmdNeedsMaintenance) ? "true" : "false";
    out += ",\"heavy\":";
    out += (f & kCmdHeavy) ? "true" : "false";
    out += ",\"quiet\":";
    out += (f & kCmdQuietLed) ? "true" : "false";
    out += ",\"internal\":";
    out += (f & kCmdInternal) ? "true" : "false";
    out += ",\"status_snapshot\":";
    out += (f & kCmdStatusSnapshot) ? "true" : "false";
    out += ",\"summary\":\"";
    out += jsonEscape(table[i].summary);
    out += "\"}";
  }
  out += "]}";
  return out;
}

}  // namespace nhos
