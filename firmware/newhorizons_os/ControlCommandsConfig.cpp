// ControlServer command handlers: config_*, capabilities and the set_* setters.
//
// One member per command, registered in ControlCommandTable.cpp. A handler
// never checks maintenance mode itself: the table's kCmdNeedsMaintenance
// flag is enforced once, in processCommand(), before it runs.
#include "ControlServer.h"
#include "EspNowOtaReceiver.h"
#include <esp_timer.h>
#include <memory>
#include <new>
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
#include "Watchdog.h"
#include "VersionCompare.h"

namespace nhos {

// config_schema
String ControlServer::cmdConfigSchema(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  return ok(cmd, "config_schema", ConfigRegistry::schemaJson());
}

// config_get
String ControlServer::cmdConfigGet(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!deviceConfig_) {
    return error(cmd, "config_unavailable");
  }
  const String path = extractString(request, "path");
  if (path.isEmpty()) {
    return ok(cmd, "config_get", ConfigRegistry::valuesJson(*deviceConfig_, governor_, storage_));
  }
  String value;
  if (!ConfigRegistry::readValue(path, *deviceConfig_, governor_, storage_, value)) {
    return error(cmd, "unknown_config_path");
  }
  String data = "{\"path\":\"";
  data += jsonEscape(path);
  data += "\",\"value\":\"";
  data += jsonEscape(value);
  data += "\"}";
  return ok(cmd, "config_get", data);
}

// config_set
String ControlServer::cmdConfigSet(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  const String path = extractString(request, "path");
  const String value = extractString(request, "value");
  if (path.isEmpty()) {
    return error(cmd, "path_required");
  }
  if (!deviceConfig_) {
    return error(cmd, "config_unavailable");
  }
  const ConfigEntry* entry = ConfigRegistry::find(path);
  if (entry == nullptr) {
    return error(cmd, "unknown_config_path");
  }
  if (value.isEmpty()) {
    return error(cmd, "value_required");
  }
  // Depth-1 by construction: every table entry points at a set_* handler,
  // never back here. Asserted rather than assumed because the recursion
  // costs a second String-heavy processCommand() stack frame, and this
  // task's stack has overflowed before under exactly that pressure (see
  // EspNowPairing.h's responseFrags_ comment).
  if (strncmp(entry->command, "config_", 7) == 0) {
    return error(cmd, "config_path_not_writable");
  }
  // Validate and render before dispatching. Without this a value the
  // handler's extractor cannot parse is silently replaced by that
  // argument's compile-time default and still reported as success --
  // observed on hardware with a quoted "30" for scan.target_fps.
  String encoded;
  String encodeError;
  if (!ConfigRegistry::encodeValue(*entry, value, encoded, encodeError)) {
    return error(cmd, String("value_rejected:") + encodeError);
  }
  // Synthesise the request the owning set_* handler already accepts and
  // dispatch to it, rather than reimplementing its validation here. That
  // handler stays the single owner of range-checking, persistence and the
  // apply step, so this generic path cannot drift away from the specific
  // one.
  //
  // EVERY argument that handler consumes is included, filled from the
  // current value and overridden only for the requested path. The set_*
  // handlers default an argument they did not receive to its compile-time
  // default rather than to its current value, so a single-field write that
  // omitted its siblings would silently reset them -- e.g. writing
  // scan.target_fps alone would also snap settle_us and
  // send_every_n_frames back to their defaults.
  String synthesized = "{\"command\":\"";
  synthesized += entry->command;
  synthesized += "\"";
  const ConfigEntry* all = ConfigRegistry::entries();
  for (size_t i = 0; i < ConfigRegistry::entryCount(); ++i) {
    const ConfigEntry& sibling = all[i];
    if (strcmp(sibling.command, entry->command) != 0) {
      continue;
    }
    String rendered;
    if (strcmp(sibling.path, entry->path) == 0) {
      rendered = encoded;
    } else {
      String current;
      String siblingError;
      if (!ConfigRegistry::readValue(String(sibling.path), *deviceConfig_, governor_, storage_,
                                     current) ||
          !ConfigRegistry::encodeValue(sibling, current, rendered, siblingError)) {
        continue;
      }
    }
    synthesized += ",\"";
    synthesized += sibling.argument;
    synthesized += "\":";
    synthesized += rendered;
  }
  synthesized += "}";
  return processCommand(synthesized);
}

// capabilities
String ControlServer::cmdCapabilities(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  // What a client needs to know before talking to this device: which
  // protocol, which config surface, and which board features exist. Lets
  // the Desktop generate its settings UI instead of hardcoding it.
  String data = "{\"protocol\":\"";
  data += kProtocolName;
  data += "\",\"firmware_version\":\"";
  data += kFirmwareVersion;
  data += "\",\"hardware_model\":\"";
  data += jsonEscape(kHardwareModel);
  data += "\",\"config_schema_hash\":";
  data += String(ConfigRegistry::schemaHash());
  data += ",\"config_entry_count\":";
  data += String(static_cast<unsigned>(ConfigRegistry::entryCount()));
  // A client caches the `commands` reply and refetches it only when this
  // hash changes (v1.15.0+).
  size_t commandCount = 0;
  commandTable(commandCount);
  data += ",\"command_schema_hash\":";
  data += String(commandSchemaHash());
  data += ",\"command_count\":";
  data += String(static_cast<unsigned>(commandCount));
  data += ",\"features\":{\"magnetometer\":";
  data += NHOS_BOARD_HAS_MAG ? "true" : "false";
  data += ",\"battery_gauge\":";
  data += NHOS_BOARD_HAS_MAX17048 ? "true" : "false";
  data += ",\"action_button\":";
  data += NHOS_BOARD_HAS_BUTTON ? "true" : "false";
  data += ",\"external_led\":";
  data += NHOS_BOARD_HAS_EXT_LED ? "true" : "false";
  data += ",\"oled\":";
  data += NHOS_BOARD_HAS_OLED ? "true" : "false";
  data += ",\"gpio_wake\":";
  data += NHOS_BOARD_SUPPORTS_GPIO_WAKE ? "true" : "false";
  data += ",\"dac_vref\":";
  data += NHOS_BOARD_HAS_DAC_VREF ? "true" : "false";
  data += ",\"calibration_switch\":";
  data += NHOS_BOARD_HAS_PCA9536 ? "true" : "false";
  data += ",\"range_modes\":";
  data += NHOS_BOARD_HAS_DAC_VREF ? "true" : "false";
  data += ",\"battery_id\":";
  data += NHOS_BOARD_HAS_BATID ? "true" : "false";
  data += ",\"ota_channels\":true";
  data += "},\"subsystems\":{\"scheduler\":";
  data += scheduler_ != nullptr ? "true" : "false";
  data += ",\"services\":";
  data += services_ != nullptr ? "true" : "false";
  data += ",\"faults\":";
  data += faults_ != nullptr ? "true" : "false";
  data += ",\"health\":";
  data += lifetime_ != nullptr ? "true" : "false";
  data += ",\"procfs\":";
  data += proc_ != nullptr ? "true" : "false";
  data += ",\"airtime_arbiter\":";
  data += arbiter_ != nullptr ? "true" : "false";
  data += ",\"apps\":";
  data += apps_ != nullptr ? "true" : "false";
  data += ",\"flow_engine\":";
  data += flow_ != nullptr ? "true" : "false";
  data += ",\"app_registry\":";
  data += appRegistry_ != nullptr ? "true" : "false";
  data += ",\"app_view\":";
  data += apps_ != nullptr ? "true" : "false";
  data += "}}";
  return ok(cmd, "capabilities", data);
}

// set_power_profile
String ControlServer::cmdSetPowerProfile(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!governor_ || !storage_) {
    return error(cmd, "governor_unavailable");
  }
  const String name = extractString(request, "profile");
  if (name.isEmpty()) {
    return error(cmd, "profile_required");
  }
  const PowerProfile profile = PowerGovernor::profileFromName(name.c_str());
  // profileFromName() falls back to performance for anything it does not
  // recognise; reject rather than silently applying the wrong profile.
  if (strcmp(PowerGovernor::profileName(profile), name.c_str()) != 0) {
    return error(cmd, "profile_invalid");
  }
  governor_->setProfile(profile);
  storage_->putString("power_profile", name);
  return ok(cmd, "power_profile_updated", governor_->statusJson());
}

// set_time
String ControlServer::cmdSetTime(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!clock_) {
    return error(cmd, "clock_unavailable");
  }
  // Milliseconds since the Unix epoch. Sent as a string because a
  // millisecond epoch overflows the int the other extractors use.
  const String epochText = extractString(request, "epoch_ms");
  if (epochText.isEmpty()) {
    return error(cmd, "epoch_ms_required");
  }
  const uint64_t epochMs = strtoull(epochText.c_str(), nullptr, 10);
  // `force` marks an explicit user action ("use this computer's time");
  // without it a better source already in charge (SNTP/Hub) wins.
  const ClockSubmitResult result =
      clock_->setEpochMs(epochMs, TimeSource::Host, extractBool(request, "force", false));
  if (result == ClockSubmitResult::Rejected) {
    return error(cmd, "epoch_rejected");
  }
  if (result == ClockSubmitResult::Ignored) {
    return error(cmd, "better_source_active");
  }
  return ok(cmd, "time_set", clock_->statusJson());
}

// set_clock_config
String ControlServer::cmdSetClockConfig(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!clock_ || !deviceConfig_) {
    return error(cmd, "clock_unavailable");
  }
  const ClockConfig& current = deviceConfig_->data().clock;
  String ntpServer = extractString(request, "ntp_server");
  if (ntpServer.isEmpty()) {
    ntpServer = current.ntpServer;
  }
  const int ntpIntervalS = extractInt(request, "ntp_interval_s", static_cast<int>(current.ntpIntervalS));
  const int syncIntervalS = extractInt(request, "sync_interval_s", current.syncIntervalS);
  if (ntpIntervalS <= 0 || syncIntervalS <= 0 ||
      !deviceConfig_->setClockConfig(ntpServer, static_cast<uint32_t>(ntpIntervalS),
                                     static_cast<uint32_t>(syncIntervalS))) {
    return error(cmd, "clock_config_invalid");
  }
  if (!deviceConfig_->save(*storage_)) {
    return error(cmd, "config_write_failed");
  }
  clock_->configure(*deviceConfig_);
  return ok(cmd, "clock_config_updated", clockStatusData());
}

// set_scan_timing
String ControlServer::cmdSetScanTiming(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  uint16_t fps = static_cast<uint16_t>(extractInt(request, "target_fps", kDefaultTargetFps));
  uint16_t settle = static_cast<uint16_t>(extractInt(request, "settle_us", kDefaultSettleUs));
  uint16_t sendEvery = static_cast<uint16_t>(extractInt(request, "send_every_n_frames", kDefaultSendEveryNFrames));
  if (!scanner_->setTiming(fps, settle, sendEvery)) {
    return error(cmd, "scan_timing_invalid");
  }
  if (imu_) imu_->setServiceIntervalUs(scanner_->scanIntervalUs());
  if (deviceConfig_) {
    deviceConfig_->setScanTiming(fps, settle, sendEvery);
    if (!deviceConfig_->save(*storage_)) {
      return error(cmd, "config_write_failed");
    }
  }
  return ok(cmd, "scan_timing_updated", scanTimingStatusJson());
}

// set_transport
String ControlServer::cmdSetTransport(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  // New Horizons Direct device migration (plan section 5): lets a Hub
  // (over ESP-NOW) or the existing Gateway/Desktop path (over UDP)
  // switch this device between wifi_udp and espnow transport modes.
  // Takes effect on next boot -- deliberately does not attempt a live
  // in-place transport swap, which would need to tear down/reinit
  // WiFiUDP or ESP-NOW mid-loop.
  if (!deviceConfig_) {
    return error(cmd, "config_unavailable");
  }
  const String mode = extractString(request, "mode");
  const String hubMac = extractString(request, "hub_mac");
  if (!deviceConfig_->setTransport(mode, hubMac)) {
    return error(cmd, "transport_invalid");
  }
  // Clear any stale ESP-NOW pairing-timeout flag from an unrelated past
  // failure -- otherwise a device freshly (and correctly) reconfigured
  // here to a new, reachable Hub would bounce straight to the WiFi
  // portal on its very next boot because of leftover state. Mirrors
  // WifiManager.cpp's portal-driven "Direct/Hub" path, which does the
  // same for the same reason.
  if (storage_) {
    storage_->putUInt(kEspNowPairingFailFlagKey, 0);
  }
  if (!deviceConfig_->save(*storage_)) {
    return error(cmd, "config_write_failed");
  }
  return ok(cmd, "transport_updated_reboot_required", deviceConfig_->transportJson());
}

// set_stream_buffer
String ControlServer::cmdSetStreamBuffer(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!deviceConfig_) {
    return error(cmd, "config_unavailable");
  }
  const bool enabled = extractBool(request, "enabled", deviceConfig_->data().streamBuffer.enabled);
  String mode = extractString(request, "mode");
  if (mode.isEmpty()) {
    mode = deviceConfig_->data().streamBuffer.mode;
  }
  if (!deviceConfig_->setStreamBuffer(enabled, mode)) {
    return error(cmd, "stream_buffer_invalid");
  }
  if (!scanner_->setStreamBufferConfig(deviceConfig_->data().streamBuffer.enabled, deviceConfig_->data().streamBuffer.depthFrames)) {
    return error(cmd, "stream_buffer_invalid");
  }
  if (!deviceConfig_->save(*storage_)) {
    return error(cmd, "config_write_failed");
  }
  String data = "{";
  bool first = true;
  jsonRawField(data, "stream_buffer", deviceConfig_->streamBufferJson(), first);
  jsonRawField(data, "scan_health", scanner_->healthJson(), first);
  String runtime = "{";
  bool runtimeFirst = true;
  jsonRawField(runtime, "stream_buffer", deviceConfig_->streamBufferJson(), runtimeFirst);
  runtime += "}";
  jsonRawField(data, "runtime", runtime, first);
  data += "}";
  return ok(cmd, "stream_buffer_updated", data);
}

// set_matrix_layout
String ControlServer::cmdSetMatrixLayout(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  uint8_t rows[kRows];
  uint8_t cols[kCols];
  size_t rowCount = extractArray(request, "analog_pins", rows, kRows);
  size_t colCount = extractArray(request, "select_pins", cols, kCols);
  if (!scanner_->setLayout(rows, rowCount, cols, colCount)) {
    return error(cmd, "matrix_layout_invalid");
  }
  if (deviceConfig_) {
    deviceConfig_->setMatrixLayout(rows, rowCount, cols, colCount);
    if (!deviceConfig_->save(*storage_)) {
      return error(cmd, "config_write_failed");
    }
  }
  if (calibration_) {
    calibration_->setLayout(rows, rowCount, cols, colCount);
  }
  if (boot_->mode() == RunMode::Normal && scanner_->hasLayout() && !scanner_->active() &&
      !(services_ && services_->isOperatorStopped("scanner"))) {
    scanner_->start();
    Serial.println(F("scan_task_started_by_layout_update"));
  }
  return ok(cmd, "matrix_layout_updated", layoutStatusJson());
}

// set_wifi
String ControlServer::cmdSetWifi(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  String ssid = extractString(request, "ssid");
  String password = extractString(request, "password");
  if (!wifi_->applyCredentials(ssid, password)) {
    return error(cmd, "wifi_connect_failed");
  }
  return ok(cmd, "wifi_updated", wifi_->statusJson());
}

// set_charge_profile
String ControlServer::cmdSetChargeProfile(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!power_) {
    return error(cmd, "power_unavailable");
  }
  if (!power_->supportsChargeProfiles()) {
    String data = "{\"battery\":";
    data += power_->statusJson();
    data += "}";
    return ok(cmd, "charge_profile_unsupported", data);
  }
  const String profile = extractString(request, "profile");
  if (!power_->applyProfileByName(profile)) {
    String reason = power_->supportsChargeProfiles() ? "charge_profile_failed" : "charge_profile_unsupported";
    return error(cmd, reason);
  }
  storage_->putString("charge_profile", power_->profileName());
  String data = "{\"battery\":";
  data += power_->statusJson();
  data += "}";
  return ok(cmd, "charge_profile_updated", data);
}

// set_action_button
String ControlServer::cmdSetActionButton(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
#if !NHOS_BOARD_HAS_CONFIGURABLE_BUTTON
  return error(cmd, "action_button_unsupported");
#else
  if (!deviceConfig_) {
    return error(cmd, "config_unavailable");
  }
  const String shortPress = extractString(request, "short_press");
  const String longPress = extractString(request, "long_press");
  const ActionButtonConfig previous = deviceConfig_->data().actionButton;
  if (shortPress.isEmpty() || longPress.isEmpty() ||
      !deviceConfig_->setActionButtonActions(shortPress, longPress)) {
    return error(cmd, "action_button_invalid");
  }
  if (!deviceConfig_->save(*storage_)) {
    deviceConfig_->setActionButtonActions(previous.shortPress, previous.longPress);
    return error(cmd, "config_write_failed");
  }
  String data = "{\"action_button\":";
  data += actionButtonStatusJson();
  data += "}";
  return ok(cmd, "action_button_updated", data);
#endif
}

// set_filter
String ControlServer::cmdSetFilter(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!deviceConfig_ || !scanner_) {
    return error(cmd, "config_unavailable");
  }
  const bool enabled = extractBool(request, "enabled", deviceConfig_->data().filter.enabled);
  const uint8_t median = static_cast<uint8_t>(extractInt(request, "median", deviceConfig_->data().filter.median));
  const float alpha = extractFloat(request, "alpha", deviceConfig_->data().filter.alpha);
  if (!deviceConfig_->setFilter(enabled, median, alpha) || !scanner_->setFilterConfig(deviceConfig_->data().filter)) {
    return error(cmd, "filter_invalid");
  }
  if (!deviceConfig_->save(*storage_)) {
    return error(cmd, "config_write_failed");
  }
  String data = "{";
  bool first = true;
  jsonRawField(data, "filter", deviceConfig_->filterJson(), first);
  jsonRawField(data, "config", deviceConfig_->statusJson(), first);
  data += "}";
  return ok(cmd, "filter_updated", data);
}

// set_raw_adc
String ControlServer::cmdSetRawAdc(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!deviceConfig_ || !scanner_) {
    return error(cmd, "config_unavailable");
  }
  const bool enabled = extractBool(request, "enabled", deviceConfig_->data().streamRawAdc);
  deviceConfig_->setStreamRawAdc(enabled);
  scanner_->setStreamRawAdc(enabled);
  if (!deviceConfig_->save(*storage_)) {
    return error(cmd, "config_write_failed");
  }
  String data = "{";
  bool first = true;
  jsonBoolField(data, "stream_raw_adc", deviceConfig_->data().streamRawAdc, first);
  jsonRawField(data, "config", deviceConfig_->statusJson(), first);
  data += "}";
  return ok(cmd, "raw_adc_updated", data);
}

// set_battery_profile
String ControlServer::cmdSetBatteryProfile(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!batteryProfileCommandSupported(NHOS_BOARD_HAS_MAX17048)) {
    // Old boards instantiate the manager for uniform status reporting, but
    // do not have a gauge-backed battery profile or a safe manual target.
    // Reject before touching NVS so no latent cross-board setting persists.
    return error(cmd, "battery_profile_unavailable");
  }
  if (!batteryGauge_ || !storage_) {
    return error(cmd, "battery_profile_unavailable");
  }
  const int capacityMah = extractInt(request, "capacity_mah", 0);
  const int maxChargeCurrentMa = extractInt(request, "max_charge_current_ma", 0);
  if (capacityMah <= 0 || capacityMah > 65535 || maxChargeCurrentMa <= 0 ||
      maxChargeCurrentMa > 65535 ||
      !validManualBatteryProfile(static_cast<uint16_t>(capacityMah),
                                 static_cast<uint16_t>(maxChargeCurrentMa))) {
    return error(cmd, "battery_profile_invalid");
  }
  const ManualBatteryProfile profile{
      static_cast<uint16_t>(capacityMah),
      static_cast<uint16_t>(maxChargeCurrentMa), true};
  // These dedicated NVS keys deliberately bypass DeviceConfig: a battery
  // identity is hardware safety data, not a transport/UI preference.
  // Keys within NVS's 15-character limit: the original
  // battery_profile_capacity_mah / _max_charge_current_ma were too long, so
  // the write always failed and the profile was gone after every reboot.
  // Nothing was ever stored under the old names, so there is no migration.
  storage_->putUInt("bat_cap_mah", profile.capacityMah);
  storage_->putUInt("bat_max_chg_ma", profile.maxChargeCurrentMa);
  batteryGauge_->setManualProfile(profile);
  String data = "{";
  bool first = true;
  jsonRawField(data, "battery", batteryStatusJson(), first);
  jsonRawField(data, "battery_gauge", batteryGauge_->statusJson(), first);
  data += "}";
  return ok(cmd, "battery_profile_updated", data);
}

// set_imu
String ControlServer::cmdSetImu(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  const bool enabled = extractBool(request, "enabled", true);
  if (deviceConfig_) {
    deviceConfig_->setImuEnabled(enabled);
    if (!deviceConfig_->save(*storage_)) {
      return error(cmd, "config_write_failed");
    }
  }
  if (services_ && services_->isOperatorStopped("imu")) {
    // Persisting "on" is an explicit start; persisting "off" leaves the
    // runtime stop as it is (the IMU is already down).
    if (enabled) {
      services_->start("imu");
    }
  } else if (imu_) {
    imu_->setEnabled(enabled);
  }
  String data = "{\"imu\":";
  data += imu_ ? imu_->statusJson() : "{}";
  data += ",\"config\":";
  data += deviceConfig_ ? deviceConfig_->statusJson() : "{}";
  data += "}";
  return ok(cmd, "config_stored", data);
}

// set_log
String ControlServer::cmdSetLog(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!deviceConfig_) {
    return error(cmd, "config_unavailable");
  }
  // With a tag, this sets that subsystem's level only and leaves the
  // persisted global logging config alone -- per-tag levels are a runtime
  // debugging lever, not device configuration.
  const String tag = extractString(request, "tag");
  if (!tag.isEmpty()) {
    const String tagLevel = extractString(request, "level");
    if (tagLevel.isEmpty()) {
      return error(cmd, "level_required");
    }
    if (!storage_->setTagLevel(tag.c_str(), Storage::parseLogLevel(tagLevel))) {
      return error(cmd, "tag_level_rejected");
    }
    return ok(cmd, "log_tag_level_updated", storage_->tagLevelsJson());
  }
  const bool enabled = extractBool(request, "enabled", deviceConfig_->data().logging.enabled);
  String level = extractString(request, "level");
  if (level.isEmpty()) {
    level = deviceConfig_->data().logging.level;
  }
  String mode = extractString(request, "mode");
  if (mode.isEmpty()) {
    mode = deviceConfig_->data().logging.mode;
  }
  size_t maxBytes = static_cast<size_t>(extractInt(request, "max_bytes", mode == "extended" ? kExtendedLogMaxBytes : kDefaultLogMaxBytes));
  if (!deviceConfig_->setLogging(enabled, level, mode, maxBytes)) {
    return error(cmd, "log_config_invalid");
  }
  if (!deviceConfig_->save(*storage_)) {
    return error(cmd, "config_write_failed");
  }
  storage_->configureLog(
      deviceConfig_->data().logging.enabled,
      deviceConfig_->data().logging.maxBytes,
      deviceConfig_->data().logging.level);
  String data = "{\"logging\":";
  data += deviceConfig_->loggingJson();
  data += ",\"log_status\":";
  data += storage_->logStatusJson();
  data += ",\"config\":";
  data += deviceConfig_->statusJson();
  data += "}";
  return ok(cmd, "log_config_updated", data);
}

// set_indicators
String ControlServer::cmdSetIndicators(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!deviceConfig_) {
    return error(cmd, "config_unavailable");
  }
  DeviceConfigData next = deviceConfig_->data();
  const String boardLed = extractObject(request, "board_led");
  if (!boardLed.isEmpty()) {
    next.boardLed.brightness = extractFloat(
        boardLed, "brightness", next.boardLed.brightness);
  }
  const String batteryLed = extractObject(request, "battery_led");
  if (!batteryLed.isEmpty()) {
#if NHOS_BOARD_HAS_BATTERY_LED
    const int threshold = extractInt(
        batteryLed, "low_battery_threshold_percent",
        next.batteryLed.lowBatteryThresholdPercent);
    if (threshold < 0 || threshold > 25) {
      return error(cmd, "battery_led_threshold_invalid");
    }
    next.batteryLed.lowBatteryThresholdPercent =
        static_cast<uint8_t>(threshold);
#else
    return error(cmd, "battery_led_unsupported");
#endif
  }
  const String external = extractObject(request, "external_led");
  const bool externalTouched = !external.isEmpty();
  if (!external.isEmpty()) {
    String mode = extractString(external, "mode");
    if (mode.isEmpty()) {
      mode = next.externalLed.mode;
    }
    if (!DeviceConfig::validExternalLedMode(mode)) {
      return error(cmd, "external_led_mode_invalid");
    }
    String preset = extractString(external, "preset");
    if (preset.isEmpty()) {
      preset = next.externalLed.preset;
    }
    String color = extractString(external, "color");
    if (color.isEmpty()) {
      color = next.externalLed.color;
    }
    next.externalLed.mode = mode;
    next.externalLed.preset = preset;
    next.externalLed.color = color;
    next.externalLed.brightness = extractFloat(external, "brightness", next.externalLed.brightness);
  }
  const String oled = extractObject(request, "oled");
  if (!oled.isEmpty()) {
    String mode = extractString(oled, "mode");
    if (mode.isEmpty()) {
      mode = next.oled.mode;
    }
    if (!DeviceConfig::validOledMode(mode)) {
      return error(cmd, "oled_mode_invalid");
    }
    next.oled.mode = mode;
    String page = extractString(oled, "page");
    if (!page.isEmpty()) {
      next.oled.page = page;
    }
    next.oled.updateHz = static_cast<uint8_t>(extractInt(oled, "update_hz", next.oled.updateHz));
    next.oled.contrast = static_cast<uint8_t>(extractInt(oled, "contrast", next.oled.contrast));
    uint8_t oledRotation = static_cast<uint8_t>(extractInt(oled, "rotation", next.oled.rotation));
    if (oledRotation > 3) oledRotation = 0;
    next.oled.rotation = oledRotation;
  }
  if (!deviceConfig_->setExternalLed(next.externalLed.mode, next.externalLed.preset, next.externalLed.brightness, next.externalLed.color)) {
    return error(cmd, "external_led_config_invalid");
  }
  if (!deviceConfig_->setBoardLedBrightness(next.boardLed.brightness)) {
    return error(cmd, "board_led_config_invalid");
  }
  if (!deviceConfig_->setBatteryLedLowBatteryThreshold(
          next.batteryLed.lowBatteryThresholdPercent)) {
    return error(cmd, "battery_led_threshold_invalid");
  }
  if (!deviceConfig_->setOled(next.oled.mode, next.oled.page, next.oled.updateHz, next.oled.contrast, next.oled.rotation)) {
    return error(cmd, "oled_config_invalid");
  }
  if (!deviceConfig_->save(*storage_)) {
    return error(cmd, "config_write_failed");
  }
  if (externalLeds_) {
    externalLeds_->apply(deviceConfig_->data().externalLed);
    if (externalTouched && deviceConfig_->data().externalLed.mode == "enabled") {
      externalLeds_->identify();
    }
  }
  if (leds_) {
    leds_->setBrightness(deviceConfig_->data().boardLed.brightness);
  }
  if (display_) {
    display_->apply(deviceConfig_->data().oled);
  }
  String data = "{\"indicators\":";
  data += indicatorsStatusJson();
  data += ",\"config\":";
  data += deviceConfig_->statusJson();
  data += "}";
  return ok(cmd, "config_stored", data);
}

// set_ota_config
String ControlServer::cmdSetOtaConfig(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!deviceConfig_) {
    return error(cmd, "config_unavailable");
  }
  const bool autoApplyOnBoot = extractBool(request, "auto_apply_on_boot", deviceConfig_->data().ota.autoApplyOnBoot);
  // Absent keeps the current value. A present-but-empty manifest_url
  // clears the custom URL so the device follows its channel again.
  String manifestUrl = deviceConfig_->data().ota.manifestUrl;
  jsonExtractString(request, "manifest_url", manifestUrl);
  String channel = deviceConfig_->data().ota.channel;
  jsonExtractString(request, "channel", channel);
  if (!isValidUpdateChannel(channel.c_str())) {
    return error(cmd, "invalid_update_channel");
  }
  if (!deviceConfig_->setOtaConfig(autoApplyOnBoot, manifestUrl, channel)) {
    return error(cmd, "ota_config_invalid");
  }
  if (!deviceConfig_->save(*storage_)) {
    return error(cmd, "config_write_failed");
  }
  String data = "{\"ota\":";
  data += deviceConfig_->otaJson();
  data += ",\"update_state\":";
  data += ota_->lastStatusJson();
  data += ",\"config\":";
  data += deviceConfig_->statusJson();
  data += "}";
  return ok(cmd, "ota_config_updated", data);
}

// commands
String ControlServer::cmdCommands(const CommandContext& ctx) {
  return ok(ctx.cmd, "commands", commandsJson());
}

// set_vref (v2.0.F): Vref goes to the DAC register only. The EEPROM -- what
// the chip powers up with -- is repaired at boot and never touched here, so a
// reboot, a reset or a power cycle always returns to 0.33 V.
String ControlServer::cmdSetVref(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
#if !NHOS_BOARD_HAS_DAC_VREF
  (void)request;
  return error(cmd, "vref_unsupported");
#else
  if (!vref_ || !vref_->available()) {
    return error(cmd, "vref_unavailable");
  }
  const int limitMv = extractInt(request, "limit_mv", -1);
  if (limitMv >= 0) {
    if (limitMv < kVrefSafeMv || limitMv > kVrefAbsoluteMaxMv) {
      return error(cmd, "vref_limit_invalid");
    }
    vref_->setLimitMv(static_cast<uint32_t>(limitMv));
  }
  const int mv = extractInt(request, "mv", -1);
  if (mv >= 0) {
    const char* failure = "vref_set_failed";
    if (!vref_->setVrefMv(static_cast<uint32_t>(mv), failure)) {
      return error(cmd, failure);
    }
  } else if (limitMv < 0) {
    return error(cmd, "vref_mv_required");
  }
  // The compensator must use the new Vref from the very next reading, not
  // from whenever its service() next runs (a lowered limit may move it too).
  if (range_) {
    range_->setVrefMv(static_cast<float>(vref_->vrefMv()));
  }
  String data = "{\"vref\":" + vref_->statusJson() + "}";
  return ok(cmd, "vref_updated", data);
#endif
}

String ControlServer::cmdCalibrationSwitch(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
#if !NHOS_BOARD_HAS_PCA9536
  (void)request;
  return error(cmd, "calibration_switch_unsupported");
#else
  if (!vref_ || !vref_->calibrationAvailable()) {
    return error(cmd, "calibration_switch_unavailable");
  }
  const bool on = extractBool(request, "on", false);
  const char* failure = "calibration_switch_failed";
  if (!vref_->setCalibration(on, failure)) {
    return error(cmd, failure);
  }
  String data = "{\"vref\":" + vref_->statusJson() + "}";
  return ok(cmd, on ? "calibration_on" : "calibration_off", data);
#endif
}

// set_range (v2.0.F): how a row reading is presented. raw leaves it as read;
// virtual re-expresses it as the reference Vref / Rfb would have read it, so
// stored calibrations from a 20k board stay valid whatever Rfb is soldered and
// whatever Vref is set; auto does the same and also lowers Vref when rows clip.
// Both need the measured Rfb (rfb_measure).
String ControlServer::cmdSetRange(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
#if !NHOS_BOARD_HAS_DAC_VREF
  (void)request;
  return error(cmd, "range_unsupported");
#else
  if (!range_ || !range_->enabled() || !vref_) {
    return error(cmd, "range_unavailable");
  }
  // Auto may have left Vref lowered; leaving it (by mode or by clear_rfb)
  // hands Vref back at the safe value at once, before any later check can
  // return, and a failure to do so is reported.
  const char* restoreFailure = "";
  auto restoreSafeVref = [&]() {
    const char* failure = "";
    if (!vref_->setVrefMv(kVrefSafeMv, failure)) {
      restoreFailure = failure[0] ? failure : "vref_set_failed";
    }
    range_->setVrefMv(static_cast<float>(vref_->vrefMv()));
  };
  if (extractBool(request, "clear_rfb", false)) {
    const bool wasAuto = range_->mode() == RangeMode::Auto;
    range_->clearRfb();
    range_->setMode(RangeMode::Raw);
    if (wasAuto) {
      restoreSafeVref();
    }
  }
  const int refRfb = extractInt(request, "ref_rfb_ohm", -1);
  if (refRfb >= 0 && !range_->setRefRfbOhm(static_cast<uint32_t>(refRfb))) {
    return error(cmd, "range_ref_rfb_invalid");
  }
  if (request.indexOf("\"leak_comp\"") >= 0) {
    range_->setLeakCompensation(extractBool(request, "leak_comp", range_->leakCompensation()));
  }
  const String mode = extractString(request, "mode");
  if (!mode.isEmpty()) {
    RangeMode next;
    if (mode == "raw") {
      next = RangeMode::Raw;
    } else if (mode == "virtual") {
      next = RangeMode::Virtual;
    } else if (mode == "auto") {
      next = RangeMode::Auto;
    } else {
      return error(cmd, "range_mode_invalid");
    }
    if (next != RangeMode::Raw && range_->rfbKnownRows() == 0) {
      return error(cmd, "range_rfb_required");
    }
    const bool leavingAuto = range_->mode() == RangeMode::Auto && next != RangeMode::Auto;
    range_->setMode(next);
    if (leavingAuto) {
      restoreSafeVref();
    }
  }
  range_->setVrefMv(static_cast<float>(vref_->vrefMv()));
  String data = "{\"range\":" + range_->statusJson() + ",\"vref\":" + vref_->statusJson();
  if (restoreFailure[0]) {
    data += ",\"warnings\":[\"vref_restore_failed\"],\"vref_restore_error\":\"" + String(restoreFailure) + "\"";
  }
  data += "}";
  return ok(cmd, "range_updated", data);
#endif
}

// rfb_measure (v2.0.F): with every column released a row's op-amp sees only
// Rfb and, once the calibration network is on, a known 10k:
//   Vout = Vref * (1 + Rfb / 10k).
// One Vref is not accurate enough: the ESP32 ADC is several mV off at this end
// (ADC1 and ADC2 differently), which is 2-3 % of the 330 mV baseline. So the
// first pass at the current Vref only finds out how big each Rfb is; the answer
// comes from six Vref points from 100 mV up to where the largest Rfb stays off
// the rail (about 2.6 V out); the slopes of the readings against Vref cancel the
// offset and the gain, and the fit averages out the ADC's scatter. The span must
// start low: the ADC's error is not constant over its range, and a fit over the
// top of it alone came out 1.5 % high and disagreed between ADC1 and ADC2. The row's offset is kept for the range modes. Sensors may stay
// connected: a released column draws nothing, apart from column 13's 100k.
namespace {
struct RfbPass {
  float off[kRows] = {0};
  float on[kRows] = {0};
  bool offOk[kRows] = {false};
  bool onOk[kRows] = {false};
  bool rowOk(size_t r) const { return offOk[r] && onOk[r]; }
};

// Restoring Vref / the calibration switch after a measurement: a failure is
// reported (warnings) instead of being swallowed behind an "ok".
[[maybe_unused]] void restoreVrefState(VrefManager& vref, bool calibrationOn, uint16_t vrefMv, String& warnings) {
  const char* failure = "";
  if (vref.calibrationAvailable() && !vref.setCalibration(calibrationOn, failure)) {
    warnings += String(warnings.isEmpty() ? "" : ",") + "\"calibration_switch_restore_failed\"";
  }
  failure = "";
  if (!vref.setVrefMv(vrefMv, failure)) {
    warnings += String(warnings.isEmpty() ? "" : ",") + "\"vref_restore_failed\"";
  }
}
}  // namespace

static bool measureRfbPass(VrefManager& vref, MatrixScanner& scanner, size_t rows, uint8_t samples,
                           int vrefMv, RfbPass& pass, const char*& failure) {
  if (!vref.setVrefMv(static_cast<uint32_t>(vrefMv), failure)) {
    return false;
  }
  for (int waited = 0; waited < 60; waited += 20) {
    delay(20);  // the op-amps follow the new Vref
    watchdogFeed();
  }
  if (!vref.setCalibration(false, failure)) {
    return false;
  }
  delay(30);
  if (!scanner.readIdleRows(pass.off, pass.offOk, rows, samples)) {
    failure = "rfb_measure_failed";
    return false;
  }
  if (!vref.setCalibration(true, failure)) {
    return false;
  }
  delay(40);
  const bool read = scanner.readIdleRows(pass.on, pass.onOk, rows, samples);
  const char* switchFailure = "";
  const bool switchedOff = vref.setCalibration(false, switchFailure);
  if (!read) {
    failure = "rfb_measure_failed";
    return false;
  }
  if (!switchedOff) {
    // The network would stay on and load every row: stop here.
    failure = switchFailure[0] ? switchFailure : "calibration_switch_failed";
    return false;
  }
  return true;
}

String ControlServer::cmdRfbMeasure(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
#if !NHOS_BOARD_HAS_DAC_VREF
  (void)request;
  return error(cmd, "rfb_measure_unsupported");
#else
  if (!scanner_ || !range_ || !range_->enabled() || !vref_ || !vref_->available() ||
      !vref_->calibrationAvailable()) {
    return error(cmd, "rfb_measure_unavailable");
  }
  const ScanHealth health = scanner_->health();
  const size_t rows = health.rows;
  if (rows == 0 || rows > kRows) {
    return error(cmd, "rfb_measure_no_layout");
  }
  int samples = extractInt(request, "samples", 24);
  samples = samples < 4 ? 4 : (samples > 64 ? 64 : samples);
  const int firstVref = extractInt(request, "vref_mv", kVrefSafeMv);
  const uint16_t previousVref = vref_->vrefMv();
  const bool previousCal = vref_->calibrationOn();
  const char* failure = "rfb_measure_failed";

  const bool pauseScan = !maintenanceMode() && scanner_->active();
  if (pauseScan) scanner_->stop();
  range_->setBypass(true);

  // Pass 1: how big is each Rfb?
  RfbPass first;
  bool good = measureRfbPass(*vref_, *scanner_, rows, static_cast<uint8_t>(samples), firstVref, first, failure);
  float coarse[kRows] = {0};
  float biggest = 0.0f;
  if (good) {
    for (size_t r = 0; r < rows; ++r) {
      if (!first.rowOk(r)) {
        continue;  // every ADC read of this row failed: no value, reported below
      }
      const float diff = first.off[r] > firstVref ? first.off[r] - firstVref : firstVref - first.off[r];
      const float reference = diff <= firstVref * 0.05f ? first.off[r] : static_cast<float>(firstVref);
      coarse[r] = rfbFromCalibration(first.on[r], reference);
      biggest = coarse[r] > biggest ? coarse[r] : biggest;
    }
  }

  // The fit: kFitPoints Vref values, the largest as high as the biggest Rfb allows.
  constexpr size_t kFitPoints = 6;
  static const float kFitFractions[kFitPoints] = {0.0f, 0.07f, 0.143f, 0.329f, 0.571f, 1.0f};
  float rfb[kRows] = {0};
  float offset[kRows] = {0};
  RfbPass pass[kFitPoints];
  float pointMv[kFitPoints] = {0};
  bool fitted_points = false;
  if (good && biggest > 0.0f) {
    // Never at or above the GPIO45 strap risk, whatever limit the operator set.
    const float top = rfbFitTopMv(biggest, static_cast<float>(vref_->limitMv()),
                                  static_cast<float>(kRfbFitMaxVrefMv));
    const int highMv = static_cast<int>(top);
    const int lowMv = static_cast<int>(kVrefMinMv);
    if (highMv - lowMv >= 100) {
      fitted_points = true;
      for (size_t i = 0; i < kFitPoints && good; ++i) {
        const int mv = lowMv + static_cast<int>((highMv - lowMv) * kFitFractions[i] + 0.5f);
        pointMv[i] = static_cast<float>(mv);
        good = measureRfbPass(*vref_, *scanner_, rows, static_cast<uint8_t>(samples), mv, pass[i], failure);
      }
    }
  }
  String warnings;
  restoreVrefState(*vref_, previousCal, previousVref, warnings);
  range_->setVrefMv(static_cast<float>(vref_->vrefMv()));
  range_->setBypass(false);
  if (pauseScan) scanner_->start();
  if (!good) {
    if (!warnings.isEmpty()) {
      return error(cmd, failure, "{\"warnings\":[" + warnings + "]}");
    }
    return error(cmd, failure);
  }

  String rowsJson = "[";
  size_t failedRows = 0;
  size_t fitted = 0;
  for (size_t r = 0; r < rows; ++r) {
    float result = 0.0f;
    float rowOffset = 0.0f;
    bool fitReadsOk = fitted_points;
    for (size_t i = 0; fitted_points && i < kFitPoints; ++i) {
      fitReadsOk = fitReadsOk && pass[i].rowOk(r);
    }
    if (fitReadsOk && coarse[r] > 0.0f) {
      float offs[kFitPoints], ons[kFitPoints];
      for (size_t i = 0; i < kFitPoints; ++i) {
        offs[i] = pass[i].off[r];
        ons[i] = pass[i].on[r];
      }
      result = rfbFromPoints(pointMv, offs, ons, kFitPoints, &rowOffset);
    }
    // A row the two points cannot resolve keeps the single-point value rather
    // than none; the method field says which it is.
    const bool resolved = result > 0.0f;
    if (!resolved) {
      result = coarse[r];
      rowOffset = 0.0f;
    }
    // A row whose first pass had no good ADC read stores nothing (0 = unknown).
    const bool adcFailed = !first.rowOk(r);
    if (adcFailed) {
      result = 0.0f;
      rowOffset = 0.0f;
      ++failedRows;
    }
    rfb[r] = result;
    offset[r] = rowOffset;
    const char* status = adcFailed
        ? "adc_read_failed"
        : (result > 0.0f
               ? "ok"
               : (isSaturatedMv(first.off[r]) ? "no_feedback" : (isSaturatedMv(first.on[r]) ? "saturated" : "open")));
    fitted += result > 0.0f ? 1 : 0;
    if (r) rowsJson += ",";
    rowsJson += "{\"row\":" + String(static_cast<unsigned>(r));
    rowsJson += ",\"off_mv\":" + String(first.off[r], 1);
    rowsJson += ",\"on_mv\":" + String(first.on[r], 1);
    rowsJson += ",\"rfb_ohm\":" + String(static_cast<unsigned long>(result + 0.5f));
    rowsJson += ",\"offset_mv\":" + String(rowOffset, 1);
    rowsJson += ",\"method\":\"" + String(adcFailed ? "none" : (resolved ? "fit" : "single")) + "\"";
    rowsJson += ",\"status\":\"" + String(status) + "\"}";
  }
  rowsJson += "]";
  range_->setRfbTable(rfb, offset, rows);
  range_->setVrefMv(static_cast<float>(vref_->vrefMv()));
  String data = "{\"vref_mv\":" + String(firstVref) + ",\"fitted_rows\":" + String(static_cast<unsigned>(fitted));
  data += ",\"points_mv\":[";
  for (size_t i = 0; fitted_points && i < kFitPoints; ++i) {
    data += (i ? "," : "") + String(static_cast<int>(pointMv[i]));
  }
  data += "]";
  data += ",\"failed_rows\":" + String(static_cast<unsigned>(failedRows));
  if (failedRows > 0) {
    warnings += String(warnings.isEmpty() ? "" : ",") + "\"adc_read_failed_rows\"";
  }
  if (!warnings.isEmpty()) {
    data += ",\"warnings\":[" + warnings + "]";
  }
  data += ",\"rows\":" + rowsJson + ",\"range\":" + range_->statusJson() + "}";
  return ok(cmd, "rfb_measured", data);
#endif
}

// vref_sweep (v2.0.F): holds the pressure, steps Vref, and for every step
// records the raw matrix (median over duration_ms). If a material is a pure
// resistance, Rs computed from each step -- Rs = Rfb * Vref / (Vout - Vref) --
// stays the same; how it moves with Vref is what the experiment is for. Rs is
// computed from the stored two-point Rfb and offsets (rfb_measure), never from a
// per-step Rfb: that one is only good to a few percent at low Vref, which would
// show up as a change in Rs. Compensation is bypassed meanwhile.
String ControlServer::cmdVrefSweep(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
#if !NHOS_BOARD_HAS_DAC_VREF
  (void)request;
  return error(cmd, "vref_sweep_unsupported");
#else
  if (!scanner_ || !range_ || !range_->enabled() || !vref_ || !vref_->available()) {
    return error(cmd, "vref_sweep_unavailable");
  }
  const ScanHealth health = scanner_->health();
  const size_t rows = health.rows;
  const size_t cols = health.cols;
  const size_t points = health.pointCount;
  if (rows == 0 || cols == 0 || rows > kRows || points == 0 || points > kMaxSensors) {
    return error(cmd, "vref_sweep_no_layout");
  }
  const int startMv = extractInt(request, "start_mv", kVrefMinMv);
  const int stopMv = extractInt(request, "stop_mv", vref_->limitMv());
  const int stepMv = extractInt(request, "step_mv", 100);
  int settleMs = extractInt(request, "settle_ms", 150);
  int durationMs = extractInt(request, "duration_ms", 300);
  settleMs = settleMs < 20 ? 20 : (settleMs > 1000 ? 1000 : settleMs);
  durationMs = durationMs < 100 ? 100 : (durationMs > 1000 ? 1000 : durationMs);
  if (startMv < kVrefMinMv || stopMv < startMv || stepMv < 10 || stopMv > vref_->limitMv()) {
    return error(cmd, "vref_sweep_range_invalid");
  }
  const int steps = (stopMv - startMv) / stepMv + 1;
  if (steps > 24) {
    return error(cmd, "vref_sweep_too_many_steps");
  }

  const uint16_t previousVref = vref_->vrefMv();
  const bool previousCal = vref_->calibrationOn();
  const bool pauseScan = !maintenanceMode() && scanner_->active();
  if (pauseScan) scanner_->stop();
  range_->setBypass(true);
  if (vref_->calibrationAvailable()) {
    // With the network left on every row would read the 10k as well.
    const char* switchFailure = "";
    if (!vref_->setCalibration(false, switchFailure)) {
      range_->setBypass(false);
      if (pauseScan) scanner_->start();
      return error(cmd, switchFailure[0] ? switchFailure : "calibration_switch_failed");
    }
  }

  String data;
  data.reserve(512 + static_cast<size_t>(steps) * (points * 5 + 60));
  data = "{\"rows\":" + String(static_cast<unsigned>(rows)) + ",\"cols\":" + String(static_cast<unsigned>(cols));
  data += ",\"rfb_ohm\":[";
  for (size_t r = 0; r < rows; ++r) {
    if (r) data += ",";
    data += String(static_cast<unsigned long>(range_->rfbOhm(r) + 0.5f));
  }
  data += "],\"offset_mv\":[";
  for (size_t r = 0; r < rows; ++r) {
    if (r) data += ",";
    data += String(range_->offsetMv(r), 1);
  }
  data += "],\"steps\":[";
  std::unique_ptr<float[]> matrix(new (std::nothrow) float[points]);
  const char* failure = "";
  bool good = matrix != nullptr;
  int done = 0;
  for (int s = 0; s < steps && good; ++s) {
    const int mv = startMv + s * stepMv;
    good = vref_->setVrefMv(static_cast<uint32_t>(mv), failure);
    if (!good) break;
    for (int waited = 0; waited < settleMs; waited += 20) {
      delay(20);
      watchdogFeed();
    }
    good = scanner_->captureAllBlockMedians(matrix.get(), points, static_cast<uint32_t>(durationMs));
    if (!good) {
      failure = "vref_sweep_capture_failed";
      break;
    }
    if (done) data += ",";
    data += "{\"vref_mv\":" + String(static_cast<float>(vref_->vrefMv()), 1);
    // captureAllBlockMedians is column-major (index = col * rows + row); the
    // reply is row-major like every other matrix on the wire.
    data += ",\"matrix_mv\":[";
    for (size_t r = 0; r < rows; ++r) {
      for (size_t c = 0; c < cols; ++c) {
        if (r || c) data += ",";
        data += String(static_cast<long>(matrix[c * rows + r] + 0.5f));
      }
    }
    data += "]}";
    ++done;
  }
  data += "]";

  String warnings;
  restoreVrefState(*vref_, previousCal, previousVref, warnings);
  range_->setVrefMv(static_cast<float>(vref_->vrefMv()));
  range_->setBypass(false);
  if (pauseScan) scanner_->start();
  if (!good) {
    const char* message = failure[0] ? failure : "vref_sweep_failed";
    if (!warnings.isEmpty()) {
      return error(cmd, message, "{\"warnings\":[" + warnings + "]}");
    }
    return error(cmd, message);
  }
  if (!warnings.isEmpty()) {
    data += ",\"warnings\":[" + warnings + "]";
  }
  data += "}";
  return ok(cmd, "vref_sweep", data);
#endif
}

// matrix_sample: one median matrix, to see what a range setting does to real
// readings. `raw` true bypasses the range compensator for the capture.
String ControlServer::cmdMatrixSample(const CommandContext& ctx) {
  const String& cmd = ctx.cmd;
  const String& request = ctx.request;
  if (!scanner_) {
    return error(cmd, "matrix_sample_unavailable");
  }
  const ScanHealth health = scanner_->health();
  const size_t rows = health.rows;
  const size_t cols = health.cols;
  const size_t points = health.pointCount;
  if (rows == 0 || cols == 0 || points == 0 || points > kMaxSensors) {
    return error(cmd, "matrix_sample_no_layout");
  }
  int durationMs = extractInt(request, "duration_ms", 300);
  durationMs = durationMs < 100 ? 100 : (durationMs > 2000 ? 2000 : durationMs);
  const bool raw = extractBool(request, "raw", false);
  std::unique_ptr<float[]> matrix(new (std::nothrow) float[points]);
  if (!matrix) {
    return error(cmd, "matrix_sample_no_memory");
  }
  const bool pauseScan = !maintenanceMode() && scanner_->active();
  if (pauseScan) scanner_->stop();
#if NHOS_BOARD_HAS_DAC_VREF
  if (raw && range_) range_->setBypass(true);
#endif
  const bool captured = scanner_->captureAllBlockMedians(matrix.get(), points, static_cast<uint32_t>(durationMs));
#if NHOS_BOARD_HAS_DAC_VREF
  if (range_) range_->setBypass(false);
#endif
  if (pauseScan) scanner_->start();
  if (!captured) {
    return error(cmd, "matrix_sample_failed");
  }
  String data;
  data.reserve(64 + points * 7);
  data = "{\"rows\":" + String(static_cast<unsigned>(rows)) + ",\"cols\":" + String(static_cast<unsigned>(cols));
  data += ",\"raw\":" + String(raw ? "true" : "false") + ",\"matrix\":[";
  // captureAllBlockMedians is column-major; the reply is row-major.
  for (size_t r = 0; r < rows; ++r) {
    for (size_t c = 0; c < cols; ++c) {
      if (r || c) data += ",";
      data += String(matrix[c * rows + r], 1);
    }
  }
  data += "]}";
  return ok(cmd, "matrix_sample", data);
}

}  // namespace nhos
