#include "MatrixScanner.h"

#include "BoardPins.h"

namespace nhos {

namespace {

bool timeReached(uint32_t nowUs, uint32_t dueUs) {
  return dueUs == 0 || static_cast<int32_t>(nowUs - dueUs) >= 0;
}

void putFloat(uint8_t* out, float value) {
  memcpy(out, &value, sizeof(float));
}

}  // namespace

bool MatrixScanner::begin() {
  rowCount_ = 0;
  colCount_ = 0;
  configurePins();
  return validatePinMap();
}

bool MatrixScanner::start() {
  running_ = true;
  nextScanDueUs_ = 0;
  scanWindowStartedMs_ = millis();
  scanWindowFrames_ = 0;
  return true;
}

void MatrixScanner::stop() {
  running_ = false;
  setAllColsInactive();
}

bool MatrixScanner::active() const {
  return running_;
}

bool MatrixScanner::hasLayout() const {
  return rowCount_ > 0 && colCount_ > 0;
}

bool MatrixScanner::setTiming(uint16_t targetFps, uint16_t settleUs, uint16_t sendEveryNFrames) {
  if (targetFps == 0 || targetFps > kMaxTargetFps || settleUs > 500 || sendEveryNFrames == 0 || sendEveryNFrames > 120) {
    return false;
  }
  targetFps_ = targetFps;
  settleUs_ = settleUs;
  sendEveryNFrames_ = sendEveryNFrames;
  nextScanDueUs_ = 0;
  return true;
}

bool MatrixScanner::setLayout(const uint8_t* rows, size_t rowCount, const uint8_t* cols, size_t colCount) {
  if (rowCount == 0 || colCount == 0) {
    if (rowCount != 0 || colCount != 0) {
      return false;
    }
    setAllColsInactive();
    rowCount_ = 0;
    colCount_ = 0;
    nextScanDueUs_ = 0;
    return true;
  }
  if (!rows || !cols || rowCount > kRows || colCount > kCols) {
    return false;
  }
  for (size_t i = 0; i < rowCount; ++i) {
    if (!isAllowedRowPin(rows[i])) {
      return false;
    }
  }
  for (size_t i = 0; i < colCount; ++i) {
    if (!isAllowedColPin(cols[i])) {
      return false;
    }
  }
  memcpy(rows_, rows, rowCount);
  memcpy(cols_, cols, colCount);
  rowCount_ = rowCount;
  colCount_ = colCount;
  nextScanDueUs_ = 0;
  configurePins();
  return true;
}

bool MatrixScanner::scanDue() const {
  return running_ && hasLayout() && timeReached(micros(), nextScanDueUs_);
}

size_t MatrixScanner::scanIntoPacketPayload(uint8_t* out, size_t capacity, MatrixFrame& frame) {
  const uint16_t pointCount = static_cast<uint16_t>(rowCount_ * colCount_);
  const size_t payloadBytes = static_cast<size_t>(pointCount) * sizeof(float);
  if (!running_ || !out || capacity < payloadBytes || pointCount > kMaxSensors) {
    return 0;
  }

  const uint32_t scanStartUs = micros();
  frame.seq = ++frameSeq_;
  frame.timestampMs = millis();
  frame.rows = static_cast<uint16_t>(rowCount_);
  frame.cols = static_cast<uint16_t>(colCount_);
  frame.pointCount = pointCount;

  size_t offset = 0;
  uint16_t index = 0;
  for (size_t c = 0; c < colCount_; ++c) {
    digitalWrite(cols_[c], LOW);
    delayMicroseconds(settleUs_);
    for (size_t r = 0; r < rowCount_; ++r) {
#if defined(ESP_ARDUINO_VERSION_MAJOR)
      float value = static_cast<float>(analogReadMilliVolts(rows_[r]));
#else
      float value = static_cast<float>(analogRead(rows_[r]));
#endif
      putFloat(out + offset, value);
      offset += sizeof(float);
      if (index < kMaxSensors) {
        frame.values[index++] = value;
      }
    }
    digitalWrite(cols_[c], HIGH);
  }
  setAllColsInactive();

  const uint32_t scanEndUs = micros();
  const uint32_t durationUs = scanEndUs - scanStartUs;
  const uint32_t budgetUs = scanIntervalUs();
  lastScanDurationUs_ = durationUs;
  if (durationUs > maxScanDurationUs_) {
    maxScanDurationUs_ = durationUs;
  }
  if (durationUs > budgetUs) {
    ++overrunFrames_;
  }
  ++produced_;
  updateScanFps(millis());
  scheduleNextScan(scanStartUs, scanEndUs);
  logPerformanceIfDue(millis());
  return offset;
}

bool MatrixScanner::shouldSendFrame(const MatrixFrame& frame) const {
  const uint16_t sendEvery = max<uint16_t>(1, sendEveryNFrames_);
  return sendEvery <= 1 || (frame.seq % sendEvery) == 0;
}

void MatrixScanner::recordUdpSend(bool ok, uint32_t durationUs) {
  lastUdpSendUs_ = durationUs;
  if (ok) {
    ++udpSentFrames_;
    consumed_ = udpSentFrames_;
  } else {
    ++udpSendFailures_;
  }
}

ScanHealth MatrixScanner::health() const {
  ScanHealth item;
  item.active = running_;
  item.produced = produced_;
  item.consumed = consumed_;
  item.dropped = dropped_;
  item.actualScanFps = actualScanFps_;
  item.lastScanDurationUs = lastScanDurationUs_;
  item.maxScanDurationUs = maxScanDurationUs_;
  item.budgetUs = scanIntervalUs();
  item.overrunFrames = overrunFrames_;
  item.udpSentFrames = udpSentFrames_;
  item.udpSendFailures = udpSendFailures_;
  item.lastUdpSendUs = lastUdpSendUs_;
  item.targetFps = targetFps_;
  item.settleUs = settleUs_;
  item.sendEveryNFrames = sendEveryNFrames_;
  item.pointCount = rowCount_ * colCount_;
  return item;
}

String MatrixScanner::healthJson() const {
  ScanHealth h = health();
  String out = "{";
  out += "\"scan_active\":";
  out += h.active ? "true" : "false";
  out += ",\"requested_target_fps\":";
  out += h.targetFps;
  out += ",\"settle_us\":";
  out += h.settleUs;
  out += ",\"send_every_n_frames\":";
  out += h.sendEveryNFrames;
  out += ",\"point_count\":";
  out += h.pointCount;
  out += ",\"produced_frames\":";
  out += h.produced;
  out += ",\"consumed_frames\":";
  out += h.consumed;
  out += ",\"dropped_frames\":";
  out += h.dropped;
  out += ",\"actual_scan_fps\":";
  out += h.actualScanFps;
  out += ",\"last_scan_duration_us\":";
  out += h.lastScanDurationUs;
  out += ",\"max_scan_duration_us\":";
  out += h.maxScanDurationUs;
  out += ",\"budget_us\":";
  out += h.budgetUs;
  out += ",\"overrun_frames\":";
  out += h.overrunFrames;
  out += ",\"udp_sent_frames\":";
  out += h.udpSentFrames;
  out += ",\"udp_send_failures\":";
  out += h.udpSendFailures;
  out += ",\"last_udp_send_us\":";
  out += h.lastUdpSendUs;
  out += "}";
  return out;
}

String MatrixScanner::matrixShapeJson() const {
  String out = "{\"rows\":";
  out += static_cast<unsigned int>(rowCount_);
  out += ",\"cols\":";
  out += static_cast<unsigned int>(colCount_);
  out += "}";
  return out;
}

String MatrixScanner::matrixLayoutJson() const {
  String out = "{\"analog_pins\":[";
  for (size_t i = 0; i < rowCount_; ++i) {
    if (i) {
      out += ",";
    }
    out += static_cast<unsigned int>(rows_[i]);
  }
  out += "],\"select_pins\":[";
  for (size_t i = 0; i < colCount_; ++i) {
    if (i) {
      out += ",";
    }
    out += static_cast<unsigned int>(cols_[i]);
  }
  out += "],\"active_rows\":[";
  for (size_t i = 0; i < rowCount_; ++i) {
    if (i) {
      out += ",";
    }
    out += static_cast<unsigned int>(rows_[i]);
  }
  out += "],\"active_cols\":[";
  for (size_t i = 0; i < colCount_; ++i) {
    if (i) {
      out += ",";
    }
    out += static_cast<unsigned int>(cols_[i]);
  }
  out += "]}";
  return out;
}

void MatrixScanner::configurePins() {
  for (size_t i = 0; i < rowCount_; ++i) {
    pinMode(rows_[i], INPUT);
  }
  for (size_t i = 0; i < colCount_; ++i) {
    pinMode(cols_[i], OUTPUT_OPEN_DRAIN);
    digitalWrite(cols_[i], HIGH);
  }
}

void MatrixScanner::setAllColsInactive() {
  for (size_t i = 0; i < colCount_; ++i) {
    digitalWrite(cols_[i], HIGH);
  }
}

uint32_t MatrixScanner::scanIntervalUs() const {
  return 1000000UL / static_cast<uint32_t>(max<uint16_t>(1, targetFps_));
}

void MatrixScanner::scheduleNextScan(uint32_t scanStartUs, uint32_t scanEndUs) {
  const uint32_t intervalUs = scanIntervalUs();
  const uint32_t dueUs = scanStartUs + intervalUs;
  nextScanDueUs_ = static_cast<int32_t>(scanEndUs - dueUs) > 0 ? scanEndUs : dueUs;
}

void MatrixScanner::updateScanFps(uint32_t nowMs) {
  if (scanWindowStartedMs_ == 0) {
    scanWindowStartedMs_ = nowMs;
  }
  ++scanWindowFrames_;
  const uint32_t windowMs = max<uint32_t>(1, nowMs - scanWindowStartedMs_);
  actualScanFps_ = (scanWindowFrames_ * 1000UL) / windowMs;
  if (windowMs >= 1000) {
    scanWindowStartedMs_ = nowMs;
    scanWindowFrames_ = 0;
  }
}

void MatrixScanner::logPerformanceIfDue(uint32_t nowMs) {
  if (lastPerfLogMs_ != 0 && nowMs - lastPerfLogMs_ < 5000) {
    return;
  }
  lastPerfLogMs_ = nowMs;
  Serial.print(F("scan_perf target_fps="));
  Serial.print(targetFps_);
  Serial.print(F(" actual_scan_fps="));
  Serial.print(actualScanFps_);
  Serial.print(F(" last_us="));
  Serial.print(lastScanDurationUs_);
  Serial.print(F(" max_us="));
  Serial.print(maxScanDurationUs_);
  Serial.print(F(" budget_us="));
  Serial.print(scanIntervalUs());
  Serial.print(F(" overrun_frames="));
  Serial.print(overrunFrames_);
  Serial.print(F(" udp_sent="));
  Serial.print(udpSentFrames_);
  Serial.print(F(" udp_failures="));
  Serial.print(udpSendFailures_);
  Serial.print(F(" last_udp_us="));
  Serial.println(lastUdpSendUs_);
  maxScanDurationUs_ = lastScanDurationUs_;
}

}  // namespace nhos
