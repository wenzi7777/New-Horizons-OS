#include "ControlServer.h"

#include "Config.h"

namespace nhos {

void ControlServer::begin(
    WifiManager& wifi,
    MatrixScanner& scanner,
    Storage& storage,
    BootModeManager& boot,
    OtaManager& ota,
    FindMeClient& findme,
    PowerManager& power,
    LedController& leds,
    DeviceConfig& deviceConfig,
    DisplayManager& display,
    ExternalLedController& externalLeds) {
  wifi_ = &wifi;
  scanner_ = &scanner;
  storage_ = &storage;
  boot_ = &boot;
  ota_ = &ota;
  findme_ = &findme;
  power_ = &power;
  leds_ = &leds;
  deviceConfig_ = &deviceConfig;
  display_ = &display;
  externalLeds_ = &externalLeds;
  streamHost_ = storage.getString("stream_host", "");
  streamPort_ = static_cast<uint16_t>(storage.getUInt("stream_port", kUdpStreamPort));
  server_.begin();
  server_.setNoDelay(true);
  started_ = true;
  Serial.print(F("control_server_started port="));
  Serial.println(kControlPort);
}

void ControlServer::service() {
  if (!started_) {
    return;
  }
  WiFiClient client = server_.available();
  if (!client) {
    return;
  }
  client.setTimeout(200);
  String request = client.readStringUntil('\n');
  request.trim();
  String cmd = commandName(request);
  String logCmd = cmd.isEmpty() ? String("missing") : cmd;
  String requestId = extractString(request, "request_id");
  Serial.print(F("control_command_received cmd="));
  Serial.print(logCmd);
  Serial.print(F(" request_id="));
  Serial.println(requestId);
  if (leds_) {
    leds_->showEvent(LedSignal::CommandReceived);
    leds_->service(millis());
  }
  Serial.print(F("control_command_executing cmd="));
  Serial.print(logCmd);
  Serial.print(F(" request_id="));
  Serial.println(requestId);
  const uint32_t startedMs = millis();
  String response = processCommand(request);
  const uint32_t durationMs = millis() - startedMs;
  const bool responseOk = response.indexOf("\"ok\":true") >= 0;
  String message = extractString(response, "message");
  Serial.print(F("control_command_finished cmd="));
  Serial.print(logCmd);
  Serial.print(F(" request_id="));
  Serial.print(requestId);
  Serial.print(F(" ok="));
  Serial.print(responseOk ? F("true") : F("false"));
  Serial.print(F(" duration_ms="));
  Serial.print(durationMs);
  Serial.print(F(" message="));
  Serial.println(message);
  if (leds_) {
    leds_->showEvent(responseOk ? LedSignal::CommandSuccess : LedSignal::CommandFailed);
    leds_->service(millis());
  }
  client.println(response);
  client.flush();
  client.stop();
  servicePendingApplyUpdate();
}

bool ControlServer::maintenanceMode() const {
  return boot_ && boot_->mode() != RunMode::Normal;
}

String ControlServer::streamHost() const {
  return streamHost_;
}

uint16_t ControlServer::streamPort() const {
  return streamPort_;
}

void ControlServer::servicePendingApplyUpdate() {
  if (!otaPending_) {
    return;
  }
  otaPending_ = false;
  const String manifestUrl = otaPendingManifestUrl_;
  otaPendingManifestUrl_ = "";
  Serial.println(F("control_apply_update_started"));
  if (leds_) {
    leds_->setSignal(LedSignal::OtaActive);
    leds_->service(millis());
  }
  const bool applied = ota_ && ota_->applyUpdate(manifestUrl);
  if (!applied) {
    Serial.print(F("control_apply_update_failed status="));
    Serial.println(ota_ ? ota_->lastStatusJson() : String("{}"));
    if (leds_) {
      leds_->showEvent(LedSignal::OtaError);
      leds_->service(millis());
    }
    return;
  }
  if (leds_) {
    leds_->showEvent(LedSignal::OtaSuccess);
    leds_->service(millis());
  }
  delay(100);
  ESP.restart();
}

String ControlServer::processCommand(const String& request) {
  const String cmd = commandName(request);
  if (cmd.isEmpty()) {
    return error("", "missing_command");
  }
  if (cmd == "status" || cmd == "query") {
    const ScanHealth health = scanner_->health();
    const String uid = deviceUidString();
    String data = "{";
    data += "\"device_uid\":\"";
    data += uid;
    data += "\",\"device_name\":\"New Horizons OS-";
    data += uid;
    data += "\",\"protocol\":\"";
    data += kProtocolName;
    data += "\",\"mode\":\"";
    data += boot_->modeName();
    data += "\",\"firmware_version\":\"";
    data += kFirmwareVersion;
    data += "\",\"hardware_model\":\"";
    data += kHardwareModel;
    data += "\",\"matrix_shape\":";
    data += scanner_->matrixShapeJson();
    data += ",\"matrix_layout\":";
    data += scanner_->matrixLayoutJson();
    data += ",\"runtime\":{\"scan_timing\":{\"target_fps\":";
    data += health.targetFps;
    data += ",\"settle_us\":";
    data += health.settleUs;
    data += ",\"send_every_n_frames\":";
    data += health.sendEveryNFrames;
    data += "},\"protocol\":\"";
    data += kProtocolName;
    data += "\",\"mode\":\"";
    data += boot_->modeName();
    data += "\"}";
    data += ",\"wifi\":";
    data += wifi_->statusJson();
    data += ",\"battery\":";
    data += power_ ? power_->statusJson() : "{}";
    data += ",\"config\":";
    data += deviceConfig_ ? deviceConfig_->statusJson() : "{}";
    data += ",\"logging\":";
    data += storage_ ? storage_->logStatusJson() : "{}";
    data += ",\"ota\":";
    data += deviceConfig_ ? deviceConfig_->otaJson() : "{}";
    data += ",\"update_state\":";
    data += ota_ ? ota_->lastStatusJson() : "{}";
    data += ",\"filter\":";
    data += deviceConfig_ ? deviceConfig_->filterJson() : "{}";
    data += ",\"imu\":";
    data += deviceConfig_ ? deviceConfig_->imuJson() : "{}";
    data += ",\"indicators\":";
    data += indicatorsStatusJson();
    data += ",\"scan_health\":";
    data += scanner_->healthJson();
    data += ",\"findme\":";
    data += findme_ ? findme_->statusJson() : "{}";
    data += "}";
    return ok(cmd, "status", data);
  }
  if (cmd == "memory_status") {
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
  if (cmd == "scan_health") {
    return ok(cmd, "scan_health", scanner_->healthJson());
  }
  if (cmd == "storage_status") {
    return ok(cmd, "storage_status", storage_->storageStatusJson());
  }
  if (cmd == "enter_maintenance") {
    scanner_->stop();
    boot_->enterMaintenance(false);
    return ok(cmd, "maintenance_entered");
  }
  if (cmd == "exit_maintenance") {
    boot_->exitMaintenance();
    scanner_->start();
    return ok(cmd, "maintenance_exited");
  }
  if (cmd == "set_scan_timing") {
    uint16_t fps = static_cast<uint16_t>(extractInt(request, "target_fps", kDefaultTargetFps));
    uint16_t settle = static_cast<uint16_t>(extractInt(request, "settle_us", kDefaultSettleUs));
    uint16_t sendEvery = static_cast<uint16_t>(extractInt(request, "send_every_n_frames", kDefaultSendEveryNFrames));
    if (!scanner_->setTiming(fps, settle, sendEvery)) {
      return error(cmd, "scan_timing_invalid");
    }
    if (deviceConfig_) {
      deviceConfig_->setScanTiming(fps, settle, sendEvery);
      if (!deviceConfig_->save(*storage_)) {
        return error(cmd, "config_write_failed");
      }
    }
    return ok(cmd, "scan_timing_updated", scanTimingStatusJson());
  }
  if (cmd == "set_matrix_layout") {
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
    if (boot_->mode() == RunMode::Normal && scanner_->hasLayout() && !scanner_->active()) {
      scanner_->start();
      Serial.println(F("scan_task_started_by_layout_update"));
    }
    return ok(cmd, "matrix_layout_updated", layoutStatusJson());
  }
  if (cmd == "set_wifi") {
    String ssid = extractString(request, "ssid");
    String password = extractString(request, "password");
    if (!wifi_->applyCredentials(ssid, password)) {
      return error(cmd, "wifi_connect_failed");
    }
    return ok(cmd, "wifi_updated", wifi_->statusJson());
  }
  if (cmd == "set_transport") {
    streamHost_ = extractString(request, "host");
    streamPort_ = static_cast<uint16_t>(extractInt(request, "udp_port", kUdpStreamPort));
    storage_->putString("stream_host", streamHost_);
    storage_->putUInt("stream_port", streamPort_);
    return ok(cmd, "transport_updated");
  }
  if (cmd == "findme_discover") {
    if (findme_) {
      findme_->discoverNow();
      return ok(cmd, "findme_discovery_started", findme_->statusJson());
    }
    return error(cmd, "findme_unavailable");
  }
  if (cmd == "findme_switch_gateway") {
    if (findme_) {
      findme_->switchGateway(
          extractString(request, "preferred_gateway_id"),
          extractString(request, "claim_id"),
          static_cast<uint32_t>(extractInt(request, "ttl_ms", 30000)));
      return ok(cmd, "findme_switch_started", findme_->statusJson());
    }
    return error(cmd, "findme_unavailable");
  }
  if (cmd == "set_charge_profile") {
    if (!power_) {
      return error(cmd, "power_unavailable");
    }
    const String profile = extractString(request, "profile");
    if (!power_->applyProfileByName(profile)) {
      return error(cmd, "charge_profile_failed");
    }
    storage_->putString("charge_profile", power_->profileName());
    String data = "{\"battery\":";
    data += power_->statusJson();
    data += "}";
    return ok(cmd, "charge_profile_updated", data);
  }
  if (cmd == "set_filter") {
    const bool enabled = extractBool(request, "enabled", true);
    if (deviceConfig_) {
      deviceConfig_->setFilterEnabled(enabled);
      if (!deviceConfig_->save(*storage_)) {
        return error(cmd, "config_write_failed");
      }
    }
    String data = "{\"filter\":{\"enabled\":";
    data += enabled ? "true" : "false";
    data += "},\"config\":";
    data += deviceConfig_ ? deviceConfig_->statusJson() : "{}";
    data += "}";
    return ok(cmd, "config_stored", data);
  }
  if (cmd == "set_imu") {
    const bool enabled = extractBool(request, "enabled", true);
    if (deviceConfig_) {
      deviceConfig_->setImuEnabled(enabled);
      if (!deviceConfig_->save(*storage_)) {
        return error(cmd, "config_write_failed");
      }
    }
    String data = "{\"imu\":{\"enabled\":";
    data += enabled ? "true" : "false";
    data += "},\"config\":";
    data += deviceConfig_ ? deviceConfig_->statusJson() : "{}";
    data += "}";
    return ok(cmd, "config_stored", data);
  }
  if (cmd == "set_log") {
    if (!deviceConfig_) {
      return error(cmd, "config_unavailable");
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
  if (cmd == "set_indicators") {
    if (!deviceConfig_) {
      return error(cmd, "config_unavailable");
    }
    DeviceConfigData next = deviceConfig_->data();
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
      next.externalLed.mode = mode;
      next.externalLed.preset = preset;
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
    }
    if (!deviceConfig_->setExternalLed(next.externalLed.mode, next.externalLed.preset, next.externalLed.brightness)) {
      return error(cmd, "external_led_config_invalid");
    }
    if (!deviceConfig_->setOled(next.oled.mode, next.oled.page, next.oled.updateHz, next.oled.contrast)) {
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
  if (cmd == "calibration_sample_cell" || cmd == "calibration_sample_all" || cmd == "calibration_save" ||
      cmd == "dump_calibration" || cmd == "delete_calibration_level") {
    if (!maintenanceMode()) {
      return error(cmd, "maintenance_required");
    }
    return ok(cmd, "calibration_command_accepted");
  }
  if (cmd == "file_list") {
    String scope = extractString(request, "scope");
    if (scope.isEmpty()) {
      scope = "user";
    }
    return ok(cmd, "file_list", storage_->listFiles(scope));
  }
  if (cmd == "file_read_begin") {
    if (!requireMaintenance(cmd)) {
      return error(cmd, "maintenance_required");
    }
    return fileSizeJson(cmd, extractString(request, "scope"), extractString(request, "path"));
  }
  if (cmd == "file_read_chunk") {
    if (!requireMaintenance(cmd)) {
      return error(cmd, "maintenance_required");
    }
    String scope = extractString(request, "scope");
    if (scope.isEmpty()) {
      scope = "user";
    }
    return fileChunkJson(
        cmd,
        scope,
        extractString(request, "path"),
        static_cast<size_t>(extractInt(request, "offset", 0)),
        static_cast<size_t>(extractInt(request, "length", 1024)));
  }
  if (cmd == "file_write_begin") {
    if (!requireMaintenance(cmd)) {
      return error(cmd, "maintenance_required");
    }
    writeScope_ = extractString(request, "scope");
    if (writeScope_.isEmpty()) {
      writeScope_ = "user";
    }
    writePath_ = extractString(request, "path");
    writeExpectedSize_ = static_cast<size_t>(extractInt(request, "size", 0));
    writeWritten_ = 0;
    if (writePath_.isEmpty() || !storage_->validUserPath(writePath_)) {
      return error(cmd, "invalid_path");
    }
    if (writeExpectedSize_ == 0 && !storage_->writeFile(writeScope_, writePath_, nullptr, 0, false)) {
      return error(cmd, "file_write_failed");
    }
    String data = "{\"scope\":\"";
    data += writeScope_;
    data += "\",\"path\":\"";
    data += writePath_;
    data += "\",\"size\":";
    data += String(static_cast<unsigned int>(writeExpectedSize_));
    data += "}";
    return ok(cmd, "file_write_started", data);
  }
  if (cmd == "file_write_chunk") {
    if (!requireMaintenance(cmd)) {
      return error(cmd, "maintenance_required");
    }
    const String path = extractString(request, "path");
    const size_t offset = static_cast<size_t>(extractInt(request, "offset", 0));
    if (path != writePath_ || offset != writeWritten_) {
      return error(cmd, "file_write_offset_mismatch");
    }
    std::vector<uint8_t> bytes;
    if (!decodeHex(extractString(request, "data"), bytes)) {
      return error(cmd, "invalid_hex_data");
    }
    if (!storage_->writeFile(writeScope_, writePath_, bytes.data(), bytes.size(), offset != 0)) {
      return error(cmd, "file_write_failed");
    }
    writeWritten_ += bytes.size();
    String data = "{\"scope\":\"";
    data += writeScope_;
    data += "\",\"path\":\"";
    data += writePath_;
    data += "\",\"written\":";
    data += String(static_cast<unsigned int>(writeWritten_));
    data += "}";
    return ok(cmd, "file_write_chunk_written", data);
  }
  if (cmd == "file_write_finish") {
    if (!requireMaintenance(cmd)) {
      return error(cmd, "maintenance_required");
    }
    const String path = extractString(request, "path");
    if (path != writePath_ || writeWritten_ != writeExpectedSize_) {
      return error(cmd, "file_write_incomplete");
    }
    String data = "{\"scope\":\"";
    data += writeScope_;
    data += "\",\"path\":\"";
    data += writePath_;
    data += "\",\"size\":";
    data += String(static_cast<unsigned int>(writeWritten_));
    data += "}";
    writeScope_ = "";
    writePath_ = "";
    writeExpectedSize_ = 0;
    writeWritten_ = 0;
    return ok(cmd, "file_write_finished", data);
  }
  if (cmd == "file_delete") {
    if (!requireMaintenance(cmd)) {
      return error(cmd, "maintenance_required");
    }
    String scope = extractString(request, "scope");
    if (scope.isEmpty()) {
      scope = "user";
    }
    String path = extractString(request, "path");
    if (!storage_->deleteFile(scope, path)) {
      return error(cmd, "file_delete_failed");
    }
    String data = "{\"scope\":\"";
    data += scope;
    data += "\",\"path\":\"";
    data += path;
    data += "\"}";
    return ok(cmd, "file_deleted", data);
  }
  if (cmd == "log_tail") {
    return ok(cmd, "log_tail", storage_->tailLog(extractInt(request, "max_lines", 50)));
  }
  if (cmd == "log_clear") {
    storage_->clearLog();
    return ok(cmd, "log_cleared");
  }
  if (cmd == "set_ota_config") {
    if (!deviceConfig_) {
      return error(cmd, "config_unavailable");
    }
    const bool autoApplyOnBoot = extractBool(request, "auto_apply_on_boot", deviceConfig_->data().ota.autoApplyOnBoot);
    String manifestUrl = extractString(request, "manifest_url");
    if (manifestUrl.isEmpty()) {
      manifestUrl = deviceConfig_->data().ota.manifestUrl;
    }
    if (!deviceConfig_->setOtaConfig(autoApplyOnBoot, manifestUrl)) {
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
  if (cmd == "check_update") {
    UpdateInfo info = ota_->checkUpdate(extractString(request, "manifest_url"));
    String data = "{\"available\":";
    data += info.available ? "true" : "false";
    data += ",\"version\":\"";
    data += info.version;
    data += "\",\"url\":\"";
    data += info.url;
    data += "\",\"sha256\":\"";
    data += info.sha256;
    data += "\",\"size\":";
    data += String(static_cast<unsigned int>(info.size));
    data += ",\"update_state\":";
    data += ota_->lastStatusJson();
    data += ",\"error\":\"";
    data += info.error;
    data += "\"}";
    return ok(cmd, info.error.isEmpty() ? "update_checked" : "update_check_failed", data);
  }
  if (cmd == "apply_update") {
    otaPendingManifestUrl_ = extractString(request, "manifest_url");
    otaPending_ = true;
    return ok(
        cmd,
        "update_started",
        "{\"update_state\":{\"phase\":\"downloading\",\"operation\":\"apply_update\",\"current_file\":\"firmware\",\"last_error\":\"\",\"last_result\":\"starting\",\"reboot_required\":true}}");
  }
  if (cmd == "reboot") {
    boot_->requestReboot();
    return ok(cmd, "reboot_scheduled");
  }
  return error(cmd, "unknown_command");
}

String ControlServer::ok(const String& command, const String& message, const String& data) const {
  String out = "{\"ok\":true,\"cmd\":\"";
  out += command;
  out += "\",\"message\":\"";
  out += message;
  out += "\",\"data\":";
  out += data;
  out += ",\"error\":\"\"}";
  return out;
}

String ControlServer::error(const String& command, const String& message) const {
  String out = "{\"ok\":false,\"cmd\":\"";
  out += command;
  out += "\",\"message\":\"";
  out += message;
  out += "\",\"data\":{},\"error\":\"";
  out += message;
  out += "\"}";
  return out;
}

String ControlServer::deviceUidString() const {
  uint8_t uid[6] = {0};
  wifi_->macBytes(uid);
  char out[13];
  snprintf(out, sizeof(out), "%02X%02X%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
  return String(out);
}

String ControlServer::commandName(const String& request) const {
  return extractString(request, "command");
}

String ControlServer::scanTimingStatusJson() const {
  const ScanHealth health = scanner_->health();
  String data = "{\"runtime\":{\"scan_timing\":{\"target_fps\":";
  data += health.targetFps;
  data += ",\"settle_us\":";
  data += health.settleUs;
  data += ",\"send_every_n_frames\":";
  data += health.sendEveryNFrames;
  data += "}},\"scan_health\":";
  data += scanner_->healthJson();
  data += "}";
  return data;
}

String ControlServer::layoutStatusJson() const {
  String data = "{\"matrix_shape\":";
  data += scanner_->matrixShapeJson();
  data += ",\"matrix_layout\":";
  data += scanner_->matrixLayoutJson();
  data += ",\"scan_health\":";
  data += scanner_->healthJson();
  data += "}";
  return data;
}

String ControlServer::indicatorsStatusJson() const {
  String data = "{\"status_led\":{\"role\":\"system_status\"},\"external_led\":";
  data += externalLeds_ ? externalLeds_->statusJson() : "{}";
  data += ",\"oled\":";
  data += display_ ? display_->statusJson() : "{}";
  data += "}";
  return data;
}

String ControlServer::extractString(const String& request, const char* key) const {
  String pattern = "\"" + String(key) + "\"";
  int keyIndex = request.indexOf(pattern);
  if (keyIndex < 0) {
    return "";
  }
  int colon = request.indexOf(':', keyIndex + pattern.length());
  int start = request.indexOf('"', colon + 1);
  int end = request.indexOf('"', start + 1);
  if (colon < 0 || start < 0 || end < 0) {
    return "";
  }
  return request.substring(start + 1, end);
}

int ControlServer::extractInt(const String& request, const char* key, int fallback) const {
  String pattern = "\"" + String(key) + "\"";
  int keyIndex = request.indexOf(pattern);
  if (keyIndex < 0) {
    return fallback;
  }
  int colon = request.indexOf(':', keyIndex + pattern.length());
  int end = request.indexOf(',', colon + 1);
  if (end < 0) {
    end = request.indexOf('}', colon + 1);
  }
  if (colon < 0 || end < 0) {
    return fallback;
  }
  return request.substring(colon + 1, end).toInt();
}

float ControlServer::extractFloat(const String& request, const char* key, float fallback) const {
  String pattern = "\"" + String(key) + "\"";
  int keyIndex = request.indexOf(pattern);
  if (keyIndex < 0) {
    return fallback;
  }
  int colon = request.indexOf(':', keyIndex + pattern.length());
  int end = request.indexOf(',', colon + 1);
  if (end < 0) {
    end = request.indexOf('}', colon + 1);
  }
  if (colon < 0 || end < 0) {
    return fallback;
  }
  String value = request.substring(colon + 1, end);
  value.trim();
  return value.toFloat();
}

bool ControlServer::extractBool(const String& request, const char* key, bool fallback) const {
  String value = extractString(request, key);
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  String pattern = "\"" + String(key) + "\"";
  int keyIndex = request.indexOf(pattern);
  if (keyIndex < 0) {
    return fallback;
  }
  int colon = request.indexOf(':', keyIndex + pattern.length());
  if (colon < 0) {
    return fallback;
  }
  int end = request.indexOf(',', colon + 1);
  if (end < 0) {
    end = request.indexOf('}', colon + 1);
  }
  if (end < 0) {
    return fallback;
  }
  value = request.substring(colon + 1, end);
  value.trim();
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  return fallback;
}

size_t ControlServer::extractArray(const String& request, const char* key, uint8_t* out, size_t maxCount) const {
  String pattern = "\"" + String(key) + "\"";
  int keyIndex = request.indexOf(pattern);
  if (keyIndex < 0) {
    return 0;
  }
  int start = request.indexOf('[', keyIndex);
  int end = request.indexOf(']', start + 1);
  if (start < 0 || end < 0) {
    return 0;
  }
  size_t count = 0;
  int cursor = start + 1;
  while (cursor < end && count < maxCount) {
    int sep = request.indexOf(',', cursor);
    if (sep < 0 || sep > end) {
      sep = end;
    }
    String token = request.substring(cursor, sep);
    token.trim();
    if (token.length()) {
      out[count++] = static_cast<uint8_t>(token.toInt());
    }
    cursor = sep + 1;
  }
  return count;
}

String ControlServer::extractObject(const String& request, const char* key) const {
  String pattern = "\"" + String(key) + "\"";
  int keyIndex = request.indexOf(pattern);
  if (keyIndex < 0) {
    return "";
  }
  int start = request.indexOf('{', keyIndex + pattern.length());
  if (start < 0) {
    return "";
  }
  int depth = 0;
  for (int i = start; i < request.length(); ++i) {
    const char c = request.charAt(i);
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) {
        return request.substring(start, i + 1);
      }
    }
  }
  return "";
}

bool ControlServer::requireMaintenance(const String& command) const {
  (void)command;
  return maintenanceMode();
}

String ControlServer::fileSizeJson(const String& command, const String& scopeValue, const String& path) const {
  String scope = scopeValue;
  if (scope.isEmpty()) {
    scope = "user";
  }
  if (path.isEmpty() || !storage_->validUserPath(path)) {
    return error(command, "invalid_path");
  }
  const size_t size = storage_->fileSize(scope, path);
  String data = "{\"scope\":\"";
  data += scope;
  data += "\",\"path\":\"";
  data += path;
  data += "\",\"size\":";
  data += String(static_cast<unsigned int>(size));
  data += "}";
  return ok(command, "file_read_ready", data);
}

String ControlServer::fileChunkJson(const String& command, const String& scope, const String& path, size_t offset, size_t length) const {
  if (path.isEmpty() || !storage_->validUserPath(path)) {
    return error(command, "invalid_path");
  }
  std::vector<uint8_t> bytes;
  if (!storage_->readFile(scope, path, bytes, offset, length)) {
    return error(command, "file_read_failed");
  }
  size_t nextOffset = offset + bytes.size();
  size_t totalSize = storage_->fileSize(scope, path);
  String data = "{\"scope\":\"";
  data += scope;
  data += "\",\"path\":\"";
  data += path;
  data += "\",\"offset\":";
  data += String(static_cast<unsigned int>(offset));
  data += ",\"next_offset\":";
  data += String(static_cast<unsigned int>(nextOffset));
  data += ",\"has_more\":";
  data += nextOffset < totalSize ? "true" : "false";
  data += ",\"data\":\"";
  data += encodeHex(bytes);
  data += "\"}";
  return ok(command, "file_read_chunk", data);
}

bool ControlServer::decodeHex(const String& hex, std::vector<uint8_t>& out) const {
  if ((hex.length() % 2) != 0) {
    return false;
  }
  out.clear();
  out.reserve(hex.length() / 2);
  for (size_t i = 0; i < hex.length(); i += 2) {
    char hi = hex.charAt(i);
    char lo = hex.charAt(i + 1);
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int a = nibble(hi);
    int b = nibble(lo);
    if (a < 0 || b < 0) {
      return false;
    }
    out.push_back(static_cast<uint8_t>((a << 4) | b));
  }
  return true;
}

String ControlServer::encodeHex(const std::vector<uint8_t>& data) const {
  static const char* digits = "0123456789abcdef";
  String out;
  out.reserve(data.size() * 2);
  for (uint8_t byte : data) {
    out += digits[(byte >> 4) & 0x0f];
    out += digits[byte & 0x0f];
  }
  return out;
}

}  // namespace nhos
