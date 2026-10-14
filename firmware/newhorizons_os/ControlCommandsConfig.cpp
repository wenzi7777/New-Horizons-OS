// ControlServer command handlers: config_*, capabilities and the set_* setters.
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
  if (boot_->mode() == RunMode::Normal && scanner_->hasLayout() && !scanner_->active()) {
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
#if !defined(NHOS_BOARD_V15F)
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
  if (imu_) {
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
#if defined(NHOS_BOARD_V15F)
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

}  // namespace nhos
