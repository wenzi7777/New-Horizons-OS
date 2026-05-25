#pragma once

#include <Arduino.h>

#include "Config.h"

namespace nhos {

struct MatrixFrame {
  uint32_t seq = 0;
  uint32_t timestampMs = 0;
  uint16_t rows = 0;
  uint16_t cols = 0;
  uint16_t pointCount = 0;
  float values[kMaxSensors] = {0};
};

struct ScanHealth {
  bool active = false;
  uint32_t produced = 0;
  uint32_t consumed = 0;
  uint32_t dropped = 0;
  uint32_t actualScanFps = 0;
  uint32_t lastScanDurationUs = 0;
  uint32_t maxScanDurationUs = 0;
  uint32_t budgetUs = 0;
  uint32_t overrunFrames = 0;
  uint32_t udpSentFrames = 0;
  uint32_t udpSendFailures = 0;
  uint32_t lastUdpSendUs = 0;
  uint16_t targetFps = kDefaultTargetFps;
  uint16_t settleUs = kDefaultSettleUs;
  uint16_t sendEveryNFrames = kDefaultSendEveryNFrames;
  uint16_t pointCount = kMaxSensors;
};

class MatrixScanner {
 public:
  bool begin();
  bool start();
  void stop();
  bool active() const;
  bool setTiming(uint16_t targetFps, uint16_t settleUs, uint16_t sendEveryNFrames = kDefaultSendEveryNFrames);
  bool setLayout(const uint8_t* rows, size_t rowCount, const uint8_t* cols, size_t colCount);
  bool scanDue() const;
  size_t scanIntoPacketPayload(uint8_t* out, size_t capacity, MatrixFrame& frame);
  bool shouldSendFrame(const MatrixFrame& frame) const;
  void recordUdpSend(bool ok, uint32_t durationUs);
  ScanHealth health() const;
  String healthJson() const;
  String matrixShapeJson() const;
  String matrixLayoutJson() const;

 private:
  void configurePins();
  void setAllColsInactive();
  uint32_t scanIntervalUs() const;
  void scheduleNextScan(uint32_t scanStartUs, uint32_t scanEndUs);
  void updateScanFps(uint32_t nowMs);
  void logPerformanceIfDue(uint32_t nowMs);

  volatile bool running_ = false;
  uint8_t rows_[kRows] = {0};
  uint8_t cols_[kCols] = {0};
  size_t rowCount_ = kRows;
  size_t colCount_ = kCols;
  uint16_t targetFps_ = kDefaultTargetFps;
  uint16_t settleUs_ = kDefaultSettleUs;
  uint16_t sendEveryNFrames_ = kDefaultSendEveryNFrames;
  uint32_t frameSeq_ = 0;
  uint32_t nextScanDueUs_ = 0;
  uint32_t produced_ = 0;
  uint32_t consumed_ = 0;
  uint32_t dropped_ = 0;
  uint32_t actualScanFps_ = 0;
  uint32_t scanWindowStartedMs_ = 0;
  uint32_t scanWindowFrames_ = 0;
  uint32_t lastScanDurationUs_ = 0;
  uint32_t maxScanDurationUs_ = 0;
  uint32_t overrunFrames_ = 0;
  uint32_t udpSentFrames_ = 0;
  uint32_t udpSendFailures_ = 0;
  uint32_t lastUdpSendUs_ = 0;
  uint32_t lastPerfLogMs_ = 0;
};

}  // namespace nhos
