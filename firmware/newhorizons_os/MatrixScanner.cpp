#include "MatrixScanner.h"

#include <esp_timer.h>
#if defined(ESP_ARDUINO_VERSION_MAJOR)
#include <esp_efuse_rtc_calib.h>
#include <hal/adc_types.h>
#endif

#include <memory>
#include <new>

#include "Watchdog.h"

#include "AdcCurveFit.h"
#include "BoardPins.h"
#include "Calibration.h"
#include "CalibrationCurve.h"

namespace nhos {

namespace {

// ESP32-S3: GPIO1-10 are ADC1, GPIO11-20 ADC2 (Calibration.cpp splits its
// readout settings the same way).
constexpr uint8_t kFirstAdc2Gpio = 11;

bool timeReached(uint32_t nowUs, uint32_t dueUs) {
  return dueUs == 0 || static_cast<int32_t>(nowUs - dueUs) >= 0;
}

void putFloat(uint8_t* out, float value) {
  memcpy(out, &value, sizeof(float));
}

void sortMedianValues(float* values, uint8_t count) {
  for (uint8_t i = 1; i < count; ++i) {
    const float value = values[i];
    int j = static_cast<int>(i) - 1;
    while (j >= 0 && values[j] > value) {
      values[j + 1] = values[j];
      --j;
    }
    values[j + 1] = value;
  }
}

}  // namespace

bool MatrixScanner::begin() {
  rowCount_ = 0;
  colCount_ = 0;
  clearPacketQueue();
  resetFilterState();
  configurePins();
  loadAdcCalibration();
  return validatePinMap();
}

// The calibration point analogReadMilliVolts() would build its curve from,
// read once. The rows are converted by AdcCurveFit instead: the core's curve
// overflows near the top of the range (4983 mV on a saturated ADC2 row).
// Attenuation 12 dB: the Arduino core's default, which nothing here changes.
void MatrixScanner::loadAdcCalibration() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && defined(CONFIG_IDF_TARGET_ESP32S3)
  const int version = esp_efuse_rtc_calib_get_ver();
  if (version < ESP_EFUSE_ADC_CALIB_VER_MIN || version > ESP_EFUSE_ADC_CALIB_VER_MAX) {
    return;
  }
  const adc_unit_t units[2] = {ADC_UNIT_1, ADC_UNIT_2};
  for (uint8_t i = 0; i < 2; ++i) {
    uint32_t digi = 0;
    uint32_t mv = 0;
    if (esp_efuse_rtc_calib_get_cal_voltage(version, units[i], ADC_ATTEN_DB_12, &digi, &mv) == ESP_OK &&
        mv == adc_curve::kCalVoltageMv) {
      adcCalDigi_[i] = digi;
    }
  }
#endif
}

// One row's reading for the column currently driven, in mV. cellIndex is the
// column-major sensor index, for holding the last value over a failed read.
float MatrixScanner::readCellMv(size_t rowIndex, uint16_t cellIndex) {
  const uint8_t pin = rows_[rowIndex];
#if defined(ESP_ARDUINO_VERSION_MAJOR)
  const uint8_t unit = pin >= kFirstAdc2Gpio ? 1 : 0;
  const uint32_t digi = adcCalDigi_[unit];
  if (digi == 0) {
    return static_cast<float>(analogReadMilliVolts(pin));
  }
  // A 12-bit code; the core hands back 0xFFFF when ADC2 lost arbitration
  // to Wi-Fi (adc_oneshot_read's -1, its error ignored).
  const uint16_t raw = analogRead(pin);
  if (raw > adc_curve::kMaxRaw) {
    ++adcReadErrors_[unit];
    return cellIndex < kMaxSensors ? lastCellMv_[cellIndex] : 0.0f;
  }
  const float mv = static_cast<float>(adc_curve::rawToMv(static_cast<adc_curve::Unit>(unit), digi, raw));
  if (cellIndex < kMaxSensors) {
    lastCellMv_[cellIndex] = mv;
  }
  return mv;
#else
  (void)cellIndex;
  return static_cast<float>(analogRead(pin));
#endif
}

bool MatrixScanner::start() {
  running_ = true;
  nextScanDueUs_ = 0;
  scanWindowStartedMs_ = millis();
  scanWindowFrames_ = 0;
  clearPacketQueue();
  resetFilterState();
  return true;
}

void MatrixScanner::stop() {
  running_ = false;
  lastPeak01_ = 0.0f;
  lastActiveCells_ = 0;
  clearPacketQueue();
  resetFilterState();
  setAllColsInactive();
  for (size_t i = 0; i < rowCount_; ++i) {
    pinMode(rows_[i], INPUT_PULLDOWN);
  }
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

bool MatrixScanner::setStreamBufferConfig(bool enabled, uint8_t depthFrames) {
  if (!enabled) {
    queueEnabled_ = false;
    queueDepthFrames_ = 0;
    clearPacketQueue();
    return true;
  }
  if (depthFrames != kStandardScanRingFrames && depthFrames != kExtendedScanRingFrames) {
    return false;
  }
  queueEnabled_ = true;
  queueDepthFrames_ = min<uint8_t>(depthFrames, static_cast<uint8_t>(kMaxScanRingFrames));
  clearPacketQueue();
  return true;
}

bool MatrixScanner::setFilterConfig(const FilterConfig& config) {
  if ((config.median != 1 && config.median != 3 && config.median != 5) ||
      config.alpha < kFilterAlphaMin || config.alpha > kFilterAlphaMax) {
    return false;
  }
  filterConfig_ = config;
  resetFilterState();
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
    resetFilterState();
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
  resetFilterState();
  for (size_t i = 0; i < kMaxSensors; ++i) {
    lastCellMv_[i] = 0.0f;
  }
  configurePins();
  return true;
}

void MatrixScanner::setCalibration(Calibration* calibration) {
  calibration_ = calibration;
}

bool MatrixScanner::streamBufferEnabled() const {
  return queueEnabled_ && queueDepthFrames_ > 0;
}

bool MatrixScanner::scanDue() const {
  return running_ && hasLayout() && timeReached(micros(), nextScanDueUs_);
}

void MatrixScanner::resetFilterState() {
  for (size_t i = 0; i < kMaxSensors; ++i) {
    filterStates_[i] = SensorFilterState{};
  }
}

float MatrixScanner::applyFilter(float value, uint16_t sensorIndex) {
  if (!filterConfig_.enabled || sensorIndex >= kMaxSensors || sensorIndex >= rowCount_ * colCount_) {
    return value;
  }

  SensorFilterState& state = filterStates_[sensorIndex];
  float medianValue = value;
  if (filterConfig_.median > 1) {
    if (state.medianCount < filterConfig_.median) {
      ++state.medianCount;
    }
    state.medianValues[state.medianCursor] = value;
    state.medianCursor = (state.medianCursor + 1) % filterConfig_.median;
    float ordered[kFilterMedianMax];
    for (uint8_t i = 0; i < state.medianCount; ++i) {
      ordered[i] = state.medianValues[i];
    }
    sortMedianValues(ordered, state.medianCount);
    medianValue = ordered[state.medianCount / 2];
  }

  if (!state.initialized) {
    state.initialized = true;
    state.lowpass = medianValue;
    return medianValue;
  }

  state.lowpass = (filterConfig_.alpha * medianValue) + ((1.0f - filterConfig_.alpha) * state.lowpass);
  return state.lowpass;
}

size_t MatrixScanner::scanIntoPacketPayload(uint8_t* out, size_t capacity, MatrixFrame& frame) {
  const uint16_t pointCount = static_cast<uint16_t>(rowCount_ * colCount_);
  const size_t payloadBytes = static_cast<size_t>(pointCount) * sizeof(float);
  if (!running_ || !out || capacity < payloadBytes || pointCount > kMaxSensors) {
    return 0;
  }

  const uint32_t scanStartUs = micros();
  // A due time of 0 means "scan now" (just started or retimed): there is
  // nothing to be late against.
  if (nextScanDueUs_ != 0) {
    const int32_t lateUs = static_cast<int32_t>(scanStartUs - nextScanDueUs_);
    startLateness_.record(lateUs > 0 ? static_cast<uint32_t>(lateUs) : 0, scanStartUs);
  }
  frame.seq = ++frameSeq_;
  frame.timestampUs = esp_timer_get_time();
  frame.timestampMs = millis();
  frame.rows = static_cast<uint16_t>(rowCount_);
  frame.cols = static_cast<uint16_t>(colCount_);
  frame.pointCount = pointCount;
  frame.hasRaw = streamRawAdc_;

  // Indicator metrics: empirical full-scale / active threshold used only to
  // normalize the LED pressure meter. Tune against real calibrated readings.
  float peakValue = 0.0f;
  uint16_t activeCells = 0;

  uint16_t colMajorIdx = 0;
  for (size_t c = 0; c < colCount_; ++c) {
    digitalWrite(cols_[c], LOW);
    delayMicroseconds(settleUs_);
    for (size_t r = 0; r < rowCount_; ++r) {
      float value = readCellMv(r, colMajorIdx);
      value = applyFilter(value, colMajorIdx);
      const uint16_t rowMajorIdx = static_cast<uint16_t>(r * colCount_ + c);
      // Capture the pre-calibration reading (filtered millivolts) before it is
      // overwritten by the calibration curve, so it can optionally be streamed.
      frame.rawValues[rowMajorIdx] = value;
      if (calibration_) {
        float calibratedValue = value;
        calibration_->apply(value, colMajorIdx, calibratedValue);
        value = calibratedValue;
      }
      putFloat(out + rowMajorIdx * sizeof(float), value);
      frame.values[rowMajorIdx] = value;
      if (value > peakValue) {
        peakValue = value;
      }
      if (value > kPressureActiveThreshold) {
        ++activeCells;
      }
      ++colMajorIdx;
    }
    digitalWrite(cols_[c], HIGH);
  }
  setAllColsInactive();

  float peak01 = peakValue / kPressureFullScale;
  if (peak01 < 0.0f) {
    peak01 = 0.0f;
  } else if (peak01 > 1.0f) {
    peak01 = 1.0f;
  }
  lastPeak01_ = peak01;
  lastActiveCells_ = activeCells;

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
  return payloadBytes;
}

// Both captures report the median of ~0.5 s block means (see
// calibration_curve::kCaptureBlockMs): a valve pulse or a twitch shifts one
// block, not the result.
bool MatrixScanner::captureCellBlockMedian(uint16_t sensorIndex, uint32_t durationMs, float& outValue) {
  if (!hasLayout() || sensorIndex >= rowCount_ * colCount_) {
    return false;
  }
  const uint32_t blocks = calibration_curve::captureBlockCount(durationMs);
  const uint32_t blockMs = max<uint32_t>(1, durationMs / blocks);
  float blockMeans[calibration_curve::kMaxCaptureBlocks];
  for (uint32_t b = 0; b < blocks; ++b) {
    const uint32_t startedMs = millis();
    float total = 0;
    uint32_t samples = 0;
    do {
      float value = 0;
      if (!sampleRawCell(sensorIndex, value)) {
        return false;
      }
      total += value;
      ++samples;
      // durationMs comes straight from the operator's calibration command and
      // is not clamped, so this can legitimately run past the 5s task WDT.
      watchdogFeed();
    } while ((millis() - startedMs) < blockMs);
    blockMeans[b] = total / static_cast<float>(samples);
  }
  outValue = calibration_curve::medianInPlace(blockMeans, blocks);
  return true;
}

bool MatrixScanner::captureAllBlockMedians(float* outValues, size_t count, uint32_t durationMs) {
  const size_t totalPoints = rowCount_ * colCount_;
  if (!hasLayout() || !outValues || totalPoints == 0 || count < totalPoints) {
    return false;
  }
  const uint32_t blocks = calibration_curve::captureBlockCount(durationMs);
  const uint32_t blockMs = max<uint32_t>(1, durationMs / blocks);
  // blocks x sensors means: 18 KB for a 10 s capture on a 225-sensor board,
  // only for the length of the capture.
  std::unique_ptr<float[]> blockMeans(new (std::nothrow) float[blocks * totalPoints]);
  if (!blockMeans) {
    return false;
  }
  for (uint32_t b = 0; b < blocks; ++b) {
    for (size_t i = 0; i < totalPoints; ++i) {
      captureTotalsScratch_[i] = 0;
    }
    const uint32_t startedMs = millis();
    uint32_t samples = 0;
    do {
      if (!sampleRawFrame(captureSampleScratch_, totalPoints)) {
        return false;
      }
      for (size_t i = 0; i < totalPoints; ++i) {
        captureTotalsScratch_[i] += captureSampleScratch_[i];
      }
      ++samples;
      watchdogFeed();  // same unbounded durationMs as captureCellBlockMedian()
    } while ((millis() - startedMs) < blockMs);
    for (size_t i = 0; i < totalPoints; ++i) {
      blockMeans[b * totalPoints + i] = captureTotalsScratch_[i] / static_cast<float>(samples);
    }
  }
  float column[calibration_curve::kMaxCaptureBlocks];
  for (size_t i = 0; i < totalPoints; ++i) {
    for (uint32_t b = 0; b < blocks; ++b) {
      column[b] = blockMeans[b * totalPoints + i];
    }
    outValues[i] = calibration_curve::medianInPlace(column, blocks);
  }
  return true;
}

bool MatrixScanner::shouldSendFrame(const MatrixFrame& frame) const {
  const uint16_t sendEvery = max<uint16_t>(1, sendEveryNFrames_);
  return sendEvery <= 1 || (frame.seq % sendEvery) == 0;
}

bool MatrixScanner::enqueuePacket(const uint8_t* data, size_t len, uint32_t seq, uint32_t timestampMs, uint8_t flags) {
  if (!streamBufferEnabled() || !data || len == 0 || len > kMaxPacketBytes) {
    return false;
  }
  if (queueCount_ >= queueDepthFrames_) {
    packetQueue_[queueTail_].occupied = false;
    queueTail_ = static_cast<uint8_t>((queueTail_ + 1) % queueDepthFrames_);
    if (queueCount_ > 0) {
      --queueCount_;
    }
    ++queueDroppedFrames_;
    ++dropped_;
  }

  PacketSlot& slot = packetQueue_[queueHead_];
  slot.occupied = true;
  slot.len = len;
  slot.seq = seq;
  slot.timestampMs = timestampMs;
  slot.flags = flags;
  memcpy(slot.bytes, data, len);

  queueHead_ = static_cast<uint8_t>((queueHead_ + 1) % queueDepthFrames_);
  if (queueCount_ < queueDepthFrames_) {
    ++queueCount_;
  }
  if (queueCount_ > queueMaxOccupiedFrames_) {
    queueMaxOccupiedFrames_ = queueCount_;
  }
  return true;
}

bool MatrixScanner::sendQueuedPacket(StreamTransport& transport) {
  if (!streamBufferEnabled() || queueCount_ == 0 || !transport.ready()) {
    return false;
  }

  PacketSlot& slot = packetQueue_[queueTail_];
  if (!slot.occupied || slot.len == 0) {
    slot.occupied = false;
    queueTail_ = static_cast<uint8_t>((queueTail_ + 1) % max<uint8_t>(1, queueDepthFrames_));
    if (queueCount_ > 0) {
      --queueCount_;
    }
    return false;
  }

  const uint32_t udpStartUs = micros();
  const bool sent = transport.sendFrame(slot.bytes, slot.len);
  recordUdpSend(sent, micros() - udpStartUs);

  slot.occupied = false;
  slot.len = 0;
  queueTail_ = static_cast<uint8_t>((queueTail_ + 1) % queueDepthFrames_);
  if (queueCount_ > 0) {
    --queueCount_;
  }
  return sent;
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

bool MatrixScanner::sampleRawFrame(float* outValues, size_t count) {
  const size_t totalPoints = rowCount_ * colCount_;
  if (!outValues || count < totalPoints || totalPoints == 0) {
    return false;
  }
  size_t index = 0;
  for (size_t c = 0; c < colCount_; ++c) {
    digitalWrite(cols_[c], LOW);
    delayMicroseconds(settleUs_);
    for (size_t r = 0; r < rowCount_; ++r) {
      outValues[index] = readCellMv(r, static_cast<uint16_t>(index));
      ++index;
    }
    digitalWrite(cols_[c], HIGH);
  }
  setAllColsInactive();
  return true;
}

bool MatrixScanner::sampleRawCell(uint16_t sensorIndex, float& outValue) {
  if (rowCount_ == 0) {
    return false;
  }
  const size_t rowIndex = sensorIndex % rowCount_;
  const size_t colIndex = sensorIndex / rowCount_;
  if (rowIndex >= rowCount_ || colIndex >= colCount_) {
    return false;
  }
  digitalWrite(cols_[colIndex], LOW);
  delayMicroseconds(settleUs_);
  outValue = readCellMv(rowIndex, sensorIndex);
  digitalWrite(cols_[colIndex], HIGH);
  setAllColsInactive();
  return true;
}

ScanHealth MatrixScanner::health() const {
  ScanHealth item;
  item.active = running_;
  item.rows = rowCount_;
  item.cols = colCount_;
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
  item.queueEnabled = queueEnabled_;
  item.queueDepthFrames = queueEnabled_ ? queueDepthFrames_ : 0;
  item.queueCapacityFrames = queueEnabled_ ? queueDepthFrames_ : 0;
  item.queueOccupiedFrames = queueEnabled_ ? queueCount_ : 0;
  item.queueDroppedFrames = queueDroppedFrames_;
  item.queueMaxOccupiedFrames = queueMaxOccupiedFrames_;
  item.targetFps = targetFps_;
  item.settleUs = settleUs_;
  item.sendEveryNFrames = sendEveryNFrames_;
  item.pointCount = rowCount_ * colCount_;
  item.startLateness = startLateness_;
  for (uint8_t i = 0; i < 2; ++i) {
    item.adcReadErrors[i] = adcReadErrors_[i];
    item.adcCalDigi[i] = adcCalDigi_[i];
  }
  return item;
}

String MatrixScanner::healthJson() const {
  ScanHealth h = health();
  String out = "{";
  out.reserve(540);
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
  out += ",\"queue_enabled\":";
  out += h.queueEnabled ? "true" : "false";
  out += ",\"queue_depth_frames\":";
  out += h.queueDepthFrames;
  out += ",\"queue_capacity_frames\":";
  out += h.queueCapacityFrames;
  out += ",\"queue_occupied_frames\":";
  out += h.queueOccupiedFrames;
  out += ",\"queue_dropped_frames\":";
  out += h.queueDroppedFrames;
  out += ",\"queue_max_occupied_frames\":";
  out += h.queueMaxOccupiedFrames;
  const ScanLateness& late = h.startLateness;
  out += ",\"start_lateness\":{\"samples\":";
  out += late.samples;
  out += ",\"late\":";
  out += late.lateStarts();
  out += ",\"gaps\":";
  out += late.gaps;
  out += ",\"max_us\":";
  out += late.maxUs;
  out += ",\"recent_max_us\":";
  out += late.recentMaxUs;
  out += ",\"hist\":[";
  for (uint8_t i = 0; i < ScanLateness::kBucketCount; ++i) {
    if (i > 0) out += ",";
    out += late.hist[i];
  }
  out += "],\"bounds_us\":[";
  for (uint8_t i = 0; i < ScanLateness::kBucketCount - 1; ++i) {
    if (i > 0) out += ",";
    out += ScanLateness::kBucketBoundsUs[i];
  }
  out += "]}";
  out += ",\"adc_read_errors\":[";
  out += h.adcReadErrors[0];
  out += ",";
  out += h.adcReadErrors[1];
  out += "],\"adc_cal_digi\":[";
  out += h.adcCalDigi[0];
  out += ",";
  out += h.adcCalDigi[1];
  out += "]}";
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

void MatrixScanner::clearPacketQueue() {
  queueHead_ = 0;
  queueTail_ = 0;
  queueCount_ = 0;
  queueMaxOccupiedFrames_ = 0;
  for (size_t i = 0; i < kMaxScanRingFrames; ++i) {
    packetQueue_[i].occupied = false;
    packetQueue_[i].len = 0;
    packetQueue_[i].seq = 0;
    packetQueue_[i].timestampMs = 0;
    packetQueue_[i].flags = 0;
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
#if NHOS_ENABLE_SERIAL_PERF_DIAGNOSTICS
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
#else
  (void)nowMs;
#endif
}

}  // namespace nhos
