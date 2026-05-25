#include "ControlServer.h"

#include "Config.h"

namespace nhos {

void ControlServer::begin(WifiManager& wifi, MatrixScanner& scanner, Storage& storage, BootModeManager& boot, OtaManager& ota, FindMeClient& findme) {
  wifi_ = &wifi;
  scanner_ = &scanner;
  storage_ = &storage;
  boot_ = &boot;
  ota_ = &ota;
  findme_ = &findme;
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
  client.println(response);
  client.stop();
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
    data += ",\"scan_health\":";
    data += scanner_->healthJson();
    data += ",\"findme\":";
    data += findme_ ? findme_->statusJson() : "{}";
    data += "}";
    return ok(cmd, "status", data);
  }
  if (cmd == "memory_status") {
    String data = "{\"heap_free\":";
    data += ESP.getFreeHeap();
    data += ",\"heap_largest_free_block\":";
    data += ESP.getMaxAllocHeap();
    data += "}";
    return ok(cmd, "memory_status", data);
  }
  if (cmd == "scan_health") {
    return ok(cmd, "scan_health", scanner_->healthJson());
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
  if (cmd == "set_filter" || cmd == "set_indicators" || cmd == "set_imu") {
    return ok(cmd, "config_stored");
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
  if (cmd == "check_update") {
    UpdateInfo info = ota_->checkUpdate(extractString(request, "manifest_url"));
    String data = "{\"available\":";
    data += info.available ? "true" : "false";
    data += ",\"version\":\"";
    data += info.version;
    data += "\",\"url\":\"";
    data += info.url;
    data += "\",\"error\":\"";
    data += info.error;
    data += "\"}";
    return ok(cmd, info.error.isEmpty() ? "update_checked" : "update_check_failed", data);
  }
  if (cmd == "apply_update") {
    bool applied = ota_->applyUpdate(extractString(request, "manifest_url"));
    return applied ? ok(cmd, "update_applied") : error(cmd, "update_failed");
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

bool ControlServer::extractBool(const String& request, const char* key, bool fallback) const {
  String value = extractString(request, key);
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
