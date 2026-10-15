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

namespace {
// Whether a command gets the received + success/failed flashes (~660ms).
// A caller marks what it sends on its own -- a poll, an automatic refresh --
// with "quiet":true: acknowledging those kept the LED flashing for as long as
// the poll ran, and an event flash outranks an app's colour, so the Desktop's
// live preview hid the very colour it was meant to show. The commands flagged
// kCmdQuietLed in the table (app_view, app_events) are quiet regardless, for callers from before the flag (Desktop
// v0.14.0 polls app_view unmarked). Anything an operator sends by hand still
// flashes, since that is the only confirmation the device gives.
bool acknowledgesOnLed(const String& cmd, bool quiet) {
  if (quiet) {
    return false;
  }
  // Polled commands (kCmdQuietLed) never flash; an unknown command still
  // acknowledges, so a typo is visible on the device.
  const CommandSpec* spec = ControlServer::findCommand(cmd);
  return spec == nullptr || (spec->flags & kCmdQuietLed) == 0;
}
}  // namespace

void ControlServer::begin(
    WifiManager& wifi,
    MatrixScanner& scanner,
    Storage& storage,
    BootModeManager& boot,
    OtaManager& ota,
    FindMeClient& findme,
    PowerManager& power,
    BatteryGaugeManager& batteryGauge,
    PowerStateManager& powerState,
    ImuManager& imu,
    MagnetometerManager& magnetometer,
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
  batteryGauge_ = &batteryGauge;
  powerState_ = &powerState;
  imu_ = &imu;
  magnetometer_ = &magnetometer;
  leds_ = &leds;
  deviceConfig_ = &deviceConfig;
  calibration_ = &calibration;
  display_ = &display;
  externalLeds_ = &externalLeds;
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
  const bool ackLed = acknowledgesOnLed(cmd, extractBool(request, "quiet", false));
  Serial.print(F("control_command_received cmd="));
  Serial.print(logCmd);
  Serial.print(F(" request_id="));
  Serial.println(requestId);
  if (leds_ && ackLed) {
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
  if (leds_ && ackLed) {
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
  // Pace out any queued large result first. Incoming packets are still read
  // every tick below: the Gateway's TIME_RESP shares this socket, and a
  // command arriving meanwhile is processed (its result queued) at once.
  serviceUdpReplies(udp);
  // One command per tick: after a queued one has run, new packets wait in
  // the socket for the next tick, so a scan due by then runs first.
  if (serviceDeferredUdpCommand(udp)) {
    return;
  }
  const int pktSize = udp.parsePacket();
  if (pktSize <= 0) {
    return;
  }
  // The Gateway's TIME_RESP arrives on this same socket. Stamped before the
  // read so as little local time as possible counts as path delay; the
  // device-side filter keeps the round trips where the loop polled promptly.
  const int64_t rxMonoUs = esp_timer_get_time();
  const IPAddress senderIp = udp.remoteIP();
  const uint16_t senderPort = udp.remotePort();
  constexpr int kMaxSize = 512;
  static char buf[kMaxSize];
  const int len = udp.read(buf, kMaxSize - 1);
  if (len == static_cast<int>(kEspNowTimeRespLen) &&
      static_cast<uint8_t>(buf[0]) == kEspNowTimeRespMagic) {
    if (clock_) {
      clock_->onGatewayTimeResponse(reinterpret_cast<const uint8_t*>(buf), len, rxMonoUs);
    }
    return;
  }
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
  const bool ackLed = acknowledgesOnLed(cmd, extractBool(payloadStr, "quiet", false));
  Serial.print(F("control_command_received cmd="));
  Serial.print(logCmd);
  Serial.print(F(" request_id="));
  Serial.println(requestId);
  if (leds_ && ackLed) {
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

  PendingUdpCommand command;
  command.payload = payloadStr;
  command.cmd = cmd;
  command.requestId = requestId;
  command.sender = sender;
  command.port = senderPort;
  command.ackLed = ackLed;
  command.sinceUs = micros();
  // A heavy command (status ~14 ms) waits for the gap after a scan instead of
  // delaying one. Its ack is already out, so the Gateway will not retry. Any
  // command behind a waiting one waits too: replies keep the request order.
  if (deferredUdpCount_ > 0 || (isHeavyCommand(cmd) && !heavyCommandMayRun(command.sinceUs))) {
    if (deferredUdpCount_ == kMaxDeferredUdp) {
      // Full: drain in order, then this one (never dropped).
      while (deferredUdpCount_ > 0) {
        runNextDeferredUdpCommand(udp);
      }
      executeUdpCommand(udp, command);
      return;
    }
    deferredUdp_[deferredUdpCount_++] = std::move(command);
    return;
  }
  executeUdpCommand(udp, command);
}

bool ControlServer::isHeavyCommand(const String& cmd) {
  const CommandSpec* spec = findCommand(cmd);
  return spec != nullptr && (spec->flags & kCmdHeavy) != 0;
}

bool ControlServer::heavyCommandMayRun(uint32_t deferredSinceUs) const {
  const uint32_t nowUs = micros();
  const bool scanning = scanner_ != nullptr && scanner_->active() && scanner_->hasLayout() &&
                        (scanGate_ == nullptr || scanGate_());
  const int32_t usUntilScan =
      scanning ? static_cast<int32_t>(scanner_->nextScanDueUs() - nowUs) : 0;
  return HeavyCommandPacing::mayRun(nowUs, deferredSinceUs, scanning, usUntilScan);
}

bool ControlServer::serviceDeferredUdpCommand(WiFiUDP& udp) {
  // The head of the queue runs when it may: a heavy one in the gap after a
  // scan, a light one (queued behind a heavy one, to keep the order) at once.
  if (deferredUdpCount_ == 0 ||
      (isHeavyCommand(deferredUdp_[0].cmd) && !heavyCommandMayRun(deferredUdp_[0].sinceUs))) {
    return false;
  }
  runNextDeferredUdpCommand(udp);
  return true;
}

void ControlServer::runNextDeferredUdpCommand(WiFiUDP& udp) {
  PendingUdpCommand command = std::move(deferredUdp_[0]);
  for (uint8_t i = 1; i < deferredUdpCount_; ++i) {
    deferredUdp_[i - 1] = std::move(deferredUdp_[i]);
  }
  --deferredUdpCount_;
  deferredUdp_[deferredUdpCount_] = PendingUdpCommand{};
  if (deferredUdpCount_ > 0) {
    // The new head's wait starts now: it has only just reached the front.
    deferredUdp_[0].sinceUs = micros();
  }
  executeUdpCommand(udp, command);
}

void ControlServer::executeUdpCommand(WiFiUDP& udp, const PendingUdpCommand& command) {
  const String logCmd = command.cmd.isEmpty() ? String("missing") : command.cmd;
  const String& requestId = command.requestId;
  const bool ackLed = command.ackLed;
  Serial.print(F("control_command_executing cmd="));
  Serial.print(logCmd);
  Serial.print(F(" request_id="));
  Serial.println(requestId);
  const uint32_t startedMs = millis();
  String response = processCommand(command.payload);
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
  if (leds_ && ackLed) {
    leds_->showEvent(responseOk ? LedSignal::CommandSuccess : LedSignal::CommandFailed);
    leds_->service(millis());
  }

  // Send result (split into chunks if it exceeds a single UDP datagram).
  sendUdpResult(udp, command.sender, command.port, deviceUidString(), requestId, std::move(response));
  servicePendingApplyUpdate();
}

String ControlServer::serviceEspNowCommand(const uint8_t* data, size_t len) {
  // ESP-NOW counterpart to serviceUdpCommand() above, reusing the same
  // processCommand() core. Unlike the UDP path, this takes the command
  // object directly (no outer {"type":"command","seq":...,"payload":{...}}
  // wrapper, no immediate ack-before-execute, no response chunking) --
  // EspNowPairing fragments the reply and, when it is bigger than one
  // frame, pages it (EspNowResultPager.h), and the ack/retry semantics for
  // Hub-initiated commands belong to the Hub-side dispatcher, not this
  // device-side handler.
  if (!started_ || data == nullptr || len == 0) {
    return error("", "invalid_request");
  }
  const String request(reinterpret_cast<const char*>(data), len);
  const String cmd = commandName(request);
  const String logCmd = cmd.isEmpty() ? String("missing") : cmd;
  const bool ackLed = acknowledgesOnLed(cmd, extractBool(request, "quiet", false));
  Serial.print(F("control_command_received transport=espnow cmd="));
  Serial.println(logCmd);
  if (leds_ && ackLed) {
    leds_->showEvent(LedSignal::CommandReceived);
    leds_->service(millis());
  }

  const uint32_t startedMs = millis();
  const String response = processCommand(request);
  const uint32_t durationMs = millis() - startedMs;
  const bool responseOk = response.indexOf("\"ok\":true") >= 0;
  Serial.print(F("control_command_finished transport=espnow cmd="));
  Serial.print(logCmd);
  Serial.print(F(" ok="));
  Serial.print(responseOk ? F("true") : F("false"));
  Serial.print(F(" duration_ms="));
  Serial.println(durationMs);
  if (leds_ && ackLed) {
    leds_->showEvent(responseOk ? LedSignal::CommandSuccess : LedSignal::CommandFailed);
    leds_->service(millis());
  }
  servicePendingApplyUpdate();
  return response;
}

// Large command responses (e.g. "status", ~2.5 KB) exceed a single UDP
// datagram and get lost to IP fragmentation over WiFi/Docker bridges. Split
// any oversized response into application-level chunks that each fit inside one
// datagram; the gateway reassembles them by request_id before forwarding.
namespace {
// Threshold and chunk size leave headroom for JSON-string escaping and the
// envelope so every datagram stays under the ~1472-byte UDP payload limit.
constexpr size_t kUdpInlineLimit = 1200;
constexpr size_t kUdpChunkRaw = 700;
}  // namespace

void ControlServer::sendUdpResult(WiFiUDP& udp, const String& host, uint16_t port, const String& uid, const String& requestId, String response) {
  if (response.length() <= kUdpInlineLimit) {
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

  PendingUdpReply reply;
  reply.host = host;
  reply.port = port;
  reply.uid = uid;
  reply.requestId = requestId;
  reply.totalChunks = (response.length() + kUdpChunkRaw - 1) / kUdpChunkRaw;
  reply.response = std::move(response);
  if (udpReplyCount_ < kMaxPendingUdpReplies) {
    if (udpReplyCount_ == 0) {
      udpChunkReadyUs_ = micros();
    }
    udpReplies_[udpReplyCount_++] = std::move(reply);  // no second 18 KB copy
    return;
  }
  // Queue full: the old blocking send, so nothing is ever dropped.
  Serial.println(F("udp_reply_queue_full sending_blocking"));
  for (size_t i = 0; i < reply.totalChunks; i++) {
    sendUdpChunk(udp, reply, i);
    // Small gap so the ESP32 WiFi stack does not drop back-to-back datagrams.
    delay(3);
  }
}

void ControlServer::serviceUdpReplies(WiFiUDP& udp) {
  if (udpReplyCount_ == 0) {
    return;
  }
  const uint32_t nowUs = micros();
  const bool scanning = scanner_ != nullptr && scanner_->active() && scanner_->hasLayout();
  const int32_t usUntilScan =
      scanning ? static_cast<int32_t>(scanner_->nextScanDueUs() - nowUs) : 0;
  if (!ReplyPacing::maySend(nowUs, udpChunkReadyUs_, scanning, usUntilScan)) {
    return;
  }
  PendingUdpReply& reply = udpReplies_[0];
  sendUdpChunk(udp, reply, reply.nextChunk++);
  udpChunkReadyUs_ = micros() + ReplyPacing::kChunkGapUs;
  if (reply.nextChunk < reply.totalChunks) {
    return;
  }
  for (uint8_t i = 1; i < udpReplyCount_; ++i) {
    udpReplies_[i - 1] = std::move(udpReplies_[i]);
  }
  --udpReplyCount_;
  udpReplies_[udpReplyCount_] = PendingUdpReply{};  // release the strings
}

void ControlServer::sendUdpChunk(WiFiUDP& udp, const PendingUdpReply& reply, size_t index) {
  const size_t start = index * kUdpChunkRaw;
  size_t end = start + kUdpChunkRaw;
  if (end > static_cast<size_t>(reply.response.length())) {
    end = static_cast<size_t>(reply.response.length());
  }
  const String fragment = reply.response.substring(start, end);
  String chunk;
  chunk.reserve(fragment.length() * 2 + 140);
  bool cf = true;
  chunk += '{';
  jsonStringField(chunk, "type", "result_chunk", cf);
  jsonStringField(chunk, "device_uid", reply.uid, cf);
  jsonStringField(chunk, "request_id", reply.requestId, cf);
  jsonUnsignedField(chunk, "chunk", static_cast<unsigned long>(index), cf);
  jsonUnsignedField(chunk, "chunks", static_cast<unsigned long>(reply.totalChunks), cf);
  jsonStringField(chunk, "data", fragment, cf);
  chunk += '}';
  udp.beginPacket(reply.host.c_str(), reply.port);
  udp.print(chunk);
  udp.endPacket();
}

bool ControlServer::maintenanceMode() const {
  return boot_ && boot_->mode() != RunMode::Normal;
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
  const CommandSpec* spec = findCommand(cmd);
  if (spec == nullptr) {
    return error(cmd, "unknown_command");
  }
  // The one place maintenance-only commands are refused. Before the table,
  // each handler checked this itself, in two different idioms.
  if ((spec->flags & kCmdNeedsMaintenance) != 0 && !maintenanceMode()) {
    return error(cmd, "maintenance_required");
  }
  const CommandContext ctx{cmd, request};
  return (this->*(spec->handler))(ctx);
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

namespace {

void appendRgb(String& out, const uint8_t rgb[3]) {
  out += "[";
  out += String(rgb[0]);
  out += ",";
  out += String(rgb[1]);
  out += ",";
  out += String(rgb[2]);
  out += "]";
}

// Slot names for the bits of `mask`, leaving out `except`.
void appendSlotList(String& out, const AppManager& apps, uint8_t mask, int8_t except) {
  out += "[";
  bool first = true;
  for (uint8_t i = 0; i < apps.slotCount(); ++i) {
    if ((mask & (1U << i)) == 0 || static_cast<int8_t>(i) == except) {
      continue;
    }
    if (!first) out += ",";
    first = false;
    out += "\"";
    out += jsonEscape(apps.slotName(i));
    out += "\"";
  }
  out += "]";
}

}  // namespace

// What the apps' outputs look like right now, after every composition rule:
// the OLED rows as the app page draws them (whichever page the panel is on,
// and on boards with no panel at all), the status LED as the pixel shows it,
// and the strip. Read-only and sized for an ESP-NOW reply (~1KB at most), so
// the Desktop can poll it to mirror the device.
String ControlServer::appViewJson() const {
  String out = "{\"oled\":{\"hw\":";
  out += NHOS_BOARD_HAS_OLED ? "true" : "false";
  out += ",\"page\":\"";
  out += display_ ? jsonEscape(display_->page()) : String("");
  out += "\",\"on_screen\":";
  out += display_ && display_->showingAppPage() ? "true" : "false";
  out += ",\"rows\":[";
  for (uint8_t row = 0; row < kOledRows; ++row) {
    if (row) out += ",";
    AppDisplayLine line;
    int8_t owner = -1;
    if (!apps_->displayLine(row, line, &owner)) {
      out += "null";
      continue;
    }
    out += "{\"slot\":\"";
    out += jsonEscape(apps_->slotName(static_cast<uint8_t>(owner)));
    out += "\",\"kind\":\"";
    if (line.kind == AppDisplayKind::Bar) {
      const OledBarGeometry bar = oledBarGeometry(static_cast<uint8_t>(strlen(line.label)),
                                                  line.value, line.lo, line.hi);
      out += "bar\",\"label\":\"";
      out += jsonEscape(line.label);
      out += "\",\"x0\":";
      out += String(bar.x0);
      out += ",\"width\":";
      out += String(bar.width);
      out += ",\"fill_px\":";
      out += String(bar.fillPx);
    } else {
      char text[kOledCols + 1];
      formatOledTextLine(line.label, line.value, line.digits, text);
      out += "text\",\"label\":\"";
      out += jsonEscape(line.label);
      out += "\",\"text\":\"";
      out += jsonEscape(text);
      out += "\"";
    }
    const uint8_t contenders = apps_->displayContenders(row);
    if ((contenders & ~(1U << owner)) != 0) {
      out += ",\"contended\":";
      appendSlotList(out, *apps_, contenders, owner);
    }
    out += "}";
  }
  out += "]},\"status_led\":{";
  if (leds_) {
    const LedColor shown = leds_->shownColor();
    const uint8_t rgb[3] = {shown.r, shown.g, shown.b};
    out += "\"rgb\":";
    appendRgb(out, rgb);
    out += ",\"signal\":\"";
    out += LedController::signalName(leds_->shownSignal());
    out += "\",";
  }
  uint8_t appRgb[3] = {0, 0, 0};
  int8_t ledOwner = -1;
  uint8_t requesters = 0;
  const bool held = apps_->statusLed(appRgb, ledOwner, &requesters);
  const bool shownApp = held && leds_ && leds_->appOverlayShown();
  out += "\"owner\":\"";
  out += shownApp ? jsonEscape(apps_->slotName(static_cast<uint8_t>(ledOwner))) : String("system");
  out += "\"";
  if (held) {
    out += ",\"app_request\":{\"slot\":\"";
    out += jsonEscape(apps_->slotName(static_cast<uint8_t>(ledOwner)));
    out += "\",\"rgb\":";
    appendRgb(out, appRgb);
    out += ",\"suppressed\":";
    out += shownApp ? "false" : "true";
    if ((requesters & ~(1U << ledOwner)) != 0) {
      out += ",\"contended\":";
      appendSlotList(out, *apps_, requesters, ledOwner);
    }
    out += "}";
  }
  out += "},\"ext_led\":{\"count\":";
  out += String(static_cast<unsigned>(kExternalLedCount));
  out += ",\"owner\":\"";
  out += externalLeds_ ? externalLeds_->activePreset() : "off";
  out += "\"";
#if NHOS_BOARD_HAS_EXT_LED
  AppExtLedFrame frame;
  apps_->extLedFrame(frame);
  if (frame.active) {
    uint8_t shown[kExternalLedCount][3];
    renderAppExtLeds(frame, static_cast<uint8_t>(kExternalLedCount), shown);
    out += ",\"pixels\":[";
    for (uint16_t i = 0; i < kExternalLedCount; ++i) {
      if (i) out += ",";
      appendRgb(out, shown[i]);
    }
    out += "]";
  }
#endif
  out += "}}";
  return out;
}

String ControlServer::indicatorsStatusJson() const {
  String data = "{";
  data.reserve(256);
  bool first = true;
  jsonRawField(data, "status_led", "{\"role\":\"system_status\"}", first);
  String boardLed = "{\"brightness\":";
  boardLed += String(deviceConfig_ ? deviceConfig_->data().boardLed.brightness : 0.30f, 2);
  boardLed += "}";
  jsonRawField(data, "board_led", boardLed, first);
  String batteryLed = "{\"supported\":";
#if defined(NHOS_BOARD_V15F)
  batteryLed += "true";
#else
  batteryLed += "false";
#endif
  batteryLed += ",\"low_battery_threshold_percent\":";
  batteryLed += String(static_cast<unsigned int>(
      deviceConfig_ ? deviceConfig_->data().batteryLed.lowBatteryThresholdPercent : 10));
  batteryLed += "}";
  jsonRawField(data, "battery_led", batteryLed, first);
  jsonRawField(data, "external_led", externalLeds_ ? externalLeds_->statusJson() : "{}", first);
  jsonRawField(data, "oled", display_ ? display_->statusJson() : "{}", first);
  data += "}";
  return data;
}

String ControlServer::actionButtonStatusJson() const {
  String data = "{\"supported\":";
#if defined(NHOS_BOARD_V15F)
  data += "true";
#else
  data += "false";
#endif
  data += ",\"boot_wifi_setup\":\"fixed_single_press_during_boot\"";
  data += ",\"short_press\":\"";
  data += deviceConfig_ ? deviceConfig_->data().actionButton.shortPress : "none";
  data += "\",\"long_press\":\"";
  data += deviceConfig_ ? deviceConfig_->data().actionButton.longPress : "soft_off";
  data += "\",\"runtime\":";
  data += powerState_ ? powerState_->actionButtonRuntimeJson() : "{}";
  data += "}";
  return data;
}

String ControlServer::batteryStatusJson() const {
  const String charger = power_ ? power_->statusJson() : String("{}");
  const String gauge = batteryGauge_ ? batteryGauge_->statusJson() : String("{}");
  const std::string merged = mergeBatteryStatusJson(
      std::string(charger.c_str()), std::string(gauge.c_str()));
  return String(merged.c_str());
}

String ControlServer::requestedManifestUrl(const String& request) const {
  const String explicitUrl = extractString(request, "manifest_url");
  if (!explicitUrl.isEmpty() || deviceConfig_ == nullptr) {
    return explicitUrl;
  }
  return deviceConfig_->resolvedManifestUrl();
}

String ControlServer::extractString(const String& request, const char* key) const {
  return jsonExtractString(request, key, "");
}

String ControlServer::clockStatusData() const {
  String data = "{\"clock\":";
  data += clock_ != nullptr ? clock_->statusJson() : String("{}");
  data += ",\"clock_config\":";
  data += deviceConfig_ != nullptr ? deviceConfig_->clockJson() : String("{}");
  data += "}";
  return data;
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


String ControlServer::fileSizeJson(const String& command, const String& scopeValue, const String& path) const {
  String scope = scopeValue;
  if (scope.isEmpty()) {
    scope = "user";
  }
  if (proc_ && ProcFs::isProcScope(scope)) {
    if (!proc_->exists(path)) {
      return error(command, "invalid_path");
    }
    String data = "{";
    bool procFirst = true;
    jsonStringField(data, "scope", scope, procFirst);
    jsonStringField(data, "path", path, procFirst);
    jsonUnsignedField(data, "size", static_cast<unsigned long>(proc_->size(path)), procFirst);
    data += "}";
    return ok(command, "file_read_ready", data);
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
  const bool fromProc = proc_ && ProcFs::isProcScope(scope);
  if (!fromProc && (path.isEmpty() || !storage_->validUserPath(path))) {
    return error(command, "invalid_path");
  }
  std::vector<uint8_t> bytes;
  if (fromProc) {
    if (!proc_->exists(path) || !proc_->read(path, offset, length, bytes)) {
      return error(command, "file_read_failed");
    }
  } else if (!storage_->readFile(scope, path, bytes, offset, length)) {
    return error(command, "file_read_failed");
  }
  size_t nextOffset = offset + bytes.size();
  size_t totalSize = fromProc ? proc_->size(path) : storage_->fileSize(scope, path);
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
