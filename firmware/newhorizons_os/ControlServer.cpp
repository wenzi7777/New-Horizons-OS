#include "ControlServer.h"

#include <vector>

#include "Config.h"
#include "JsonUtils.h"

namespace nhos {

void ControlServer::begin(
    WifiManager& wifi,
    MatrixScanner& scanner,
    Storage& storage,
    BootModeManager& boot,
    OtaManager& ota,
    FindMeClient& findme,
    PowerManager& power,
    PowerStateManager& powerState,
    ImuManager& imu,
    LedController& leds,
    DeviceConfig& deviceConfig,
    Calibration& calibration,
    DisplayManager& display,
    ExternalLedController& externalLeds) {
  wifi_ = &wifi;
  scanner_ = &scanner;
  storage_ = &storage;
  boot_ = &boot;
  ota_ = &ota;
  findme_ = &findme;
  power_ = &power;
  powerState_ = &powerState;
  imu_ = &imu;
  leds_ = &leds;
  deviceConfig_ = &deviceConfig;
  calibration_ = &calibration;
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

void ControlServer::serviceUdpCommand(WiFiUDP& udp) {
  if (!started_) {
    return;
  }
  const int pktSize = udp.parsePacket();
  if (pktSize <= 0) {
    return;
  }
  const IPAddress senderIp = udp.remoteIP();
  const uint16_t senderPort = udp.remotePort();
  constexpr int kMaxSize = 512;
  static char buf[kMaxSize];
  const int len = udp.read(buf, kMaxSize - 1);
  if (len <= 0 || buf[0] != '{') {
    return;
  }
  buf[len] = '\0';
  const String frame(buf, len);

  String typeStr;
  if (!jsonExtractString(frame, "type", typeStr) || typeStr != "command") {
    return;
  }

  long seq = 0;
  jsonExtractInt(frame, "seq", seq);
  const String requestId = jsonExtractString(frame, "request_id");
  String payloadStr;
  if (!jsonExtractObject(frame, "payload", payloadStr) || payloadStr.isEmpty()) {
    return;
  }

  String cmd = commandName(payloadStr);
  String logCmd = cmd.isEmpty() ? String("missing") : cmd;
  Serial.print(F("control_command_received cmd="));
  Serial.print(logCmd);
  Serial.print(F(" request_id="));
  Serial.println(requestId);
  if (leds_) {
    leds_->showEvent(LedSignal::CommandReceived);
    leds_->service(millis());
  }

  const String uid = deviceUidString();
  const String sender = senderIp.toString();

  // Send ACK before executing so the gateway stops retrying
  String ack;
  ack.reserve(100);
  bool af = true;
  ack += '{';
  jsonStringField(ack, "type", "ack", af);
  jsonStringField(ack, "device_uid", uid, af);
  jsonSignedField(ack, "ack", seq, af);
  jsonStringField(ack, "request_id", requestId, af);
  ack += '}';
  udp.beginPacket(sender.c_str(), senderPort);
  udp.print(ack);
  udp.endPacket();

  Serial.print(F("control_command_executing cmd="));
  Serial.print(logCmd);
  Serial.print(F(" request_id="));
  Serial.println(requestId);
  const uint32_t startedMs = millis();
  const String response = processCommand(payloadStr);
  const uint32_t durationMs = millis() - startedMs;
  const bool responseOk = response.indexOf("\"ok\":true") >= 0;
  const String message = extractString(response, "message");
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

  // Send result (split into chunks if it exceeds a single UDP datagram).
  sendUdpResult(udp, sender, senderPort, uid, requestId, response);
  servicePendingApplyUpdate();
}

// Large command responses (e.g. "status", ~2.5 KB) exceed a single UDP
// datagram and get lost to IP fragmentation over WiFi/Docker bridges. Split
// any oversized response into application-level chunks that each fit inside one
// datagram; the gateway reassembles them by request_id before forwarding.
void ControlServer::sendUdpResult(WiFiUDP& udp, const String& host, uint16_t port, const String& uid, const String& requestId, const String& response) {
  // Threshold and chunk size leave headroom for JSON-string escaping and the
  // envelope so every datagram stays under the ~1472-byte UDP payload limit.
  constexpr size_t kInlineLimit = 1200;
  if (response.length() <= kInlineLimit) {
    String result;
    result.reserve(response.length() + 120);
    bool rf = true;
    result += '{';
    jsonStringField(result, "type", "result", rf);
    jsonStringField(result, "device_uid", uid, rf);
    jsonStringField(result, "request_id", requestId, rf);
    jsonRawField(result, "payload", response, rf);
    result += '}';
    udp.beginPacket(host.c_str(), port);
    udp.print(result);
    udp.endPacket();
    return;
  }

  constexpr size_t kChunkRaw = 700;
  const size_t total = (response.length() + kChunkRaw - 1) / kChunkRaw;
  for (size_t i = 0; i < total; i++) {
    const size_t start = i * kChunkRaw;
    size_t end = start + kChunkRaw;
    if (end > static_cast<size_t>(response.length())) {
      end = static_cast<size_t>(response.length());
    }
    const String fragment = response.substring(start, end);
    String chunk;
    chunk.reserve(fragment.length() * 2 + 140);
    bool cf = true;
    chunk += '{';
    jsonStringField(chunk, "type", "result_chunk", cf);
    jsonStringField(chunk, "device_uid", uid, cf);
    jsonStringField(chunk, "request_id", requestId, cf);
    jsonUnsignedField(chunk, "chunk", static_cast<unsigned long>(i), cf);
    jsonUnsignedField(chunk, "chunks", static_cast<unsigned long>(total), cf);
    jsonStringField(chunk, "data", fragment, cf);
    chunk += '}';
    udp.beginPacket(host.c_str(), port);
    udp.print(chunk);
    udp.endPacket();
    // Small gap so the ESP32 WiFi stack does not drop back-to-back datagrams.
    delay(3);
  }
}

bool ControlServer::maintenanceMode() const {
  return boot_ && boot_->mode() != RunMode::Normal;
}

const String& ControlServer::streamHost() const {
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

    String data = "{";
    data.reserve(1536);
    bool first = true;
    jsonStringField(data, "device_uid", uid, first);
    jsonStringField(data, "device_name", String("New Horizons OS-") + uid, first);
    jsonStringField(data, "protocol", kProtocolName, first);
    jsonStringField(data, "mode", boot_->modeName(), first);
    jsonStringField(data, "firmware_version", kFirmwareVersion, first);
    jsonStringField(data, "hardware_model", kHardwareModel, first);
    jsonRawField(data, "matrix_shape", scanner_->matrixShapeJson(), first);
    jsonRawField(data, "matrix_layout", scanner_->matrixLayoutJson(), first);
    jsonRawField(data, "runtime", runtime, first);
    jsonRawField(data, "wifi", wifi_->statusJson(), first);
    jsonRawField(data, "battery", power_ ? power_->statusJson() : "{}", first);
    jsonRawField(data, "power", powerState_ ? powerState_->statusJson() : "{}", first);
    jsonRawField(data, "config", deviceConfig_ ? deviceConfig_->statusJson() : "{}", first);
    jsonRawField(data, "logging", storage_ ? storage_->logStatusJson() : "{}", first);
    jsonRawField(data, "ota", deviceConfig_ ? deviceConfig_->otaJson() : "{}", first);
    jsonRawField(data, "update_state", ota_ ? ota_->lastStatusJson() : "{}", first);
    jsonRawField(data, "filter", deviceConfig_ ? deviceConfig_->filterJson() : "{}", first);
    jsonRawField(data, "imu", imu_ ? imu_->statusJson() : "{}", first);
    jsonRawField(data, "stream_buffer", streamBuffer, first);
    jsonRawField(data, "calibration", calibration_ ? calibration_->statusJson(maintenanceMode()) : "{}", first);
    jsonRawField(data, "indicators", indicatorsStatusJson(), first);
    jsonRawField(data, "scan_health", scanner_->healthJson(), first);
    jsonRawField(data, "findme", findme_ ? findme_->statusJson() : "{}", first);
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
    if (imu_) imu_->setServiceIntervalUs(scanner_->scanIntervalUs());
    if (deviceConfig_) {
      deviceConfig_->setScanTiming(fps, settle, sendEvery);
      if (!deviceConfig_->save(*storage_)) {
        return error(cmd, "config_write_failed");
      }
    }
    return ok(cmd, "scan_timing_updated", scanTimingStatusJson());
  }
  if (cmd == "set_stream_buffer") {
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
    if (calibration_) {
      calibration_->setLayout(rows, rowCount, cols, colCount);
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
  if (cmd == "power_set_state") {
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
      uint8_t oledRotation = static_cast<uint8_t>(extractInt(oled, "rotation", next.oled.rotation));
      if (oledRotation > 3) oledRotation = 0;
      next.oled.rotation = oledRotation;
    }
    if (!deviceConfig_->setExternalLed(next.externalLed.mode, next.externalLed.preset, next.externalLed.brightness)) {
      return error(cmd, "external_led_config_invalid");
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
  if (cmd == "calibration_status") {
    if (!calibration_) {
      return error(cmd, "calibration_unavailable");
    }
    return ok(cmd, "calibration_status", calibration_->statusJson(maintenanceMode()));
  }
  if (cmd == "calibration_enable") {
    if (!calibration_) {
      return error(cmd, "calibration_unavailable");
    }
    String detail;
    if (!calibration_->setEnabled(true, detail)) {
      return error(cmd, detail.length() ? detail : "calibration_enable_failed");
    }
    return ok(cmd, "calibration_enabled", calibration_->statusJson(maintenanceMode()));
  }
  if (cmd == "calibration_disable") {
    if (!calibration_) {
      return error(cmd, "calibration_unavailable");
    }
    String detail;
    if (!calibration_->setEnabled(false, detail)) {
      return error(cmd, detail.length() ? detail : "calibration_disable_failed");
    }
    return ok(cmd, "calibration_disabled", calibration_->statusJson(maintenanceMode()));
  }
  if (cmd == "calibration_session_begin") {
    if (!maintenanceMode()) {
      return error(cmd, "maintenance_required");
    }
    if (!calibration_ || !calibration_->sessionBegin()) {
      return error(cmd, "calibration_session_begin_failed");
    }
    return ok(cmd, "calibration_session_started", calibration_->statusJson(maintenanceMode()));
  }
  if (cmd == "calibration_session_abort") {
    if (!maintenanceMode()) {
      return error(cmd, "maintenance_required");
    }
    if (!calibration_) {
      return error(cmd, "calibration_unavailable");
    }
    calibration_->sessionAbort();
    return ok(cmd, "calibration_session_aborted", calibration_->statusJson(maintenanceMode()));
  }
  if (cmd == "calibration_session_commit") {
    if (!maintenanceMode()) {
      return error(cmd, "maintenance_required");
    }
    if (!calibration_) {
      return error(cmd, "calibration_unavailable");
    }
    String detail;
    if (!calibration_->sessionCommit(extractBool(request, "auto_enable", false), detail)) {
      return error(cmd, detail.length() ? detail : "calibration_commit_failed");
    }
    return ok(cmd, "calibration_committed", calibration_->statusJson(maintenanceMode()));
  }
  if (cmd == "calibration_clear_profile") {
    if (!maintenanceMode()) {
      return error(cmd, "maintenance_required");
    }
    if (!calibration_ || !calibration_->clearProfile()) {
      return error(cmd, "calibration_clear_failed");
    }
    return ok(cmd, "calibration_profile_cleared", calibration_->statusJson(maintenanceMode()));
  }
  if (cmd == "calibration_dump_level") {
    if (!calibration_) {
      return error(cmd, "calibration_unavailable");
    }
    String levelJson;
    if (!calibration_->dumpLevelJson(extractFloat(request, "level", 0), levelJson)) {
      return error(cmd, "calibration_level_not_found");
    }
    return ok(cmd, "calibration_level_dump", levelJson);
  }
  if (cmd == "calibration_delete_level") {
    if (!maintenanceMode()) {
      return error(cmd, "maintenance_required");
    }
    if (!calibration_ || !calibration_->deleteLevel(extractFloat(request, "level", 0))) {
      return error(cmd, "calibration_level_delete_failed");
    }
    return ok(cmd, "calibration_level_deleted", calibration_->statusJson(maintenanceMode()));
  }
  if (cmd == "calibration_capture_cell") {
    if (!maintenanceMode()) {
      return error(cmd, "maintenance_required");
    }
    if (!calibration_ || !scanner_) {
      return error(cmd, "calibration_unavailable");
    }
    const uint16_t sensorIndex = static_cast<uint16_t>(extractInt(request, "sensor_index", -1));
    const uint32_t durationMs = static_cast<uint32_t>(extractInt(request, "duration_ms", 3000));
    const float level = extractFloat(request, "level", 0);
    float value = 0;
    if (!scanner_->captureCellAverage(sensorIndex, durationMs, value) || !calibration_->captureCell(sensorIndex, level, value)) {
      return error(cmd, "calibration_capture_failed");
    }
    String levelJson;
    calibration_->dumpLevelJson(level, levelJson);
    return ok(cmd, "calibration_cell_captured", levelJson);
  }
  if (cmd == "calibration_capture_all") {
    if (!maintenanceMode()) {
      return error(cmd, "maintenance_required");
    }
    if (!calibration_ || !scanner_) {
      return error(cmd, "calibration_unavailable");
    }
    const ScanHealth health = scanner_->health();
    std::vector<float> values(health.pointCount, 0);
    const uint32_t durationMs = static_cast<uint32_t>(extractInt(request, "duration_ms", 3000));
    const float level = extractFloat(request, "level", 0);
    if (!scanner_->captureAllAverages(values.data(), values.size(), durationMs) || !calibration_->captureAll(level, values.data(), values.size())) {
      return error(cmd, "calibration_capture_failed");
    }
    String levelJson;
    calibration_->dumpLevelJson(level, levelJson);
    return ok(cmd, "calibration_all_captured", levelJson);
  }
  if (cmd == "file_list") {
    String scope = extractString(request, "scope");
    if (scope.isEmpty()) {
      scope = "user";
    }
    return ok(cmd, "file_list", storage_->listFiles(scope));
  }
  if (cmd == "file_read_begin") {
    return fileSizeJson(cmd, extractString(request, "scope"), extractString(request, "path"));
  }
  if (cmd == "file_read_chunk") {
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
    if (storage_->isPathTooLong(writeScope_, writePath_)) {
      return error(cmd, "path_too_long");
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
  String out = "{";
  out.reserve(data.length() + command.length() + message.length() + 64);
  bool first = true;
  jsonBoolField(out, "ok", true, first);
  jsonStringField(out, "cmd", command, first);
  jsonStringField(out, "message", message, first);
  jsonRawField(out, "data", data, first);
  jsonStringField(out, "error", "", first);
  out += "}";
  return out;
}

String ControlServer::error(const String& command, const String& message) const {
  String out = "{";
  out.reserve(command.length() + message.length() * 2 + 48);
  bool first = true;
  jsonBoolField(out, "ok", false, first);
  jsonStringField(out, "cmd", command, first);
  jsonStringField(out, "message", message, first);
  jsonRawField(out, "data", "{}", first);
  jsonStringField(out, "error", message, first);
  out += "}";
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
  const String streamBuffer = deviceConfig_ ? deviceConfig_->streamBufferJson() : String("{\"enabled\":false,\"mode\":\"standard\",\"depth_frames\":0}");
  String scanTiming = "{";
  scanTiming.reserve(64);
  bool scanTimingFirst = true;
  jsonUnsignedField(scanTiming, "target_fps", health.targetFps, scanTimingFirst);
  jsonUnsignedField(scanTiming, "settle_us", health.settleUs, scanTimingFirst);
  jsonUnsignedField(scanTiming, "send_every_n_frames", health.sendEveryNFrames, scanTimingFirst);
  scanTiming += "}";

  String runtime = "{";
  bool runtimeFirst = true;
  jsonRawField(runtime, "scan_timing", scanTiming, runtimeFirst);
  jsonRawField(runtime, "stream_buffer", streamBuffer, runtimeFirst);
  runtime += "}";

  String data = "{";
  data.reserve(256);
  bool first = true;
  jsonRawField(data, "runtime", runtime, first);
  jsonRawField(data, "stream_buffer", streamBuffer, first);
  jsonRawField(data, "scan_health", scanner_->healthJson(), first);
  data += "}";
  return data;
}

String ControlServer::layoutStatusJson() const {
  String data = "{";
  data.reserve(384);
  bool first = true;
  jsonRawField(data, "matrix_shape", scanner_->matrixShapeJson(), first);
  jsonRawField(data, "matrix_layout", scanner_->matrixLayoutJson(), first);
  jsonRawField(data, "scan_health", scanner_->healthJson(), first);
  data += "}";
  return data;
}

String ControlServer::indicatorsStatusJson() const {
  String data = "{";
  data.reserve(256);
  bool first = true;
  jsonRawField(data, "status_led", "{\"role\":\"system_status\"}", first);
  jsonRawField(data, "external_led", externalLeds_ ? externalLeds_->statusJson() : "{}", first);
  jsonRawField(data, "oled", display_ ? display_->statusJson() : "{}", first);
  data += "}";
  return data;
}

String ControlServer::extractString(const String& request, const char* key) const {
  return jsonExtractString(request, key, "");
}

int ControlServer::extractInt(const String& request, const char* key, int fallback) const {
  long value = 0;
  return jsonExtractInt(request, key, value) ? static_cast<int>(value) : fallback;
}

float ControlServer::extractFloat(const String& request, const char* key, float fallback) const {
  float value = 0;
  return jsonExtractFloat(request, key, value) ? value : fallback;
}

bool ControlServer::extractBool(const String& request, const char* key, bool fallback) const {
  bool value = false;
  return jsonExtractBool(request, key, value) ? value : fallback;
}

size_t ControlServer::extractArray(const String& request, const char* key, uint8_t* out, size_t maxCount) const {
  return jsonExtractUInt8Array(request, key, out, maxCount);
}

String ControlServer::extractObject(const String& request, const char* key) const {
  String value;
  return jsonExtractObject(request, key, value) ? value : "";
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
  String data = "{";
  data.reserve(path.length() + scope.length() + 64);
  bool first = true;
  jsonStringField(data, "scope", scope, first);
  jsonStringField(data, "path", path, first);
  jsonUnsignedField(data, "size", static_cast<unsigned long>(size), first);
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
  String data = "{";
  data.reserve(path.length() + scope.length() + (bytes.size() * 2) + 96);
  bool first = true;
  jsonStringField(data, "scope", scope, first);
  jsonStringField(data, "path", path, first);
  jsonUnsignedField(data, "offset", static_cast<unsigned long>(offset), first);
  jsonUnsignedField(data, "next_offset", static_cast<unsigned long>(nextOffset), first);
  jsonBoolField(data, "has_more", nextOffset < totalSize, first);
  jsonStringField(data, "data", encodeHex(bytes), first);
  data += "}";
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
