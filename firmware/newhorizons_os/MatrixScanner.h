#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>

#include "Config.h"
#include "DeviceConfig.h"

namespace nhos {

class Calibration;

struct MatrixFrame {
  uint32_t seq = 0;
  uint32_t timestampMs = 0;
  uint16_t rows = 0;
  uint16_t cols = 0;
  uint16_t pointCount = 0;
  float values[kMaxSensors] = {0};
  float rawValues[kMaxSensors] = {0};
  bool hasRaw = false;
};

struct ScanHealth {
  bool active = false;
  uint16_t rows = 0;
  uint16_t cols = 0;
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
  bool queueEnabled = false;
  uint16_t queueDepthFrames = 0;
  uint16_t queueCapacityFrames = 0;
  uint16_t queueOccupiedFrames = 0;
  uint32_t queueDroppedFrames = 0;
  uint16_t queueMaxOccupiedFrames = 0;
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
  bool hasLayout() const;
  bool setTiming(uint16_t targetFps, uint16_t settleUs, uint16_t sendEveryNFrames = kDefaultSendEveryNFrames);
  bool setStreamBufferConfig(bool enabled, uint8_t depthFrames);
  bool setFilterConfig(const FilterConfig& config);
  bool setLayout(const uint8_t* rows, size_t rowCount, const uint8_t* cols, size_t colCount);
  void setCalibration(Calibration* calibration);
  void setStreamRawAdc(bool enabled) { streamRawAdc_ = enabled; }
  bool scanDue() const;
  size_t scanIntoPacketPayload(uint8_t* out, size_t capacity, MatrixFrame& frame);
  bool streamBufferEnabled() const;
  bool captureCellAverage(uint16_t sensorIndex, uint32_t durationMs, float& outValue);
  bool captureAllAverages(float* outValues, size_t count, uint32_t durationMs);
  bool shouldSendFrame(const MatrixFrame& frame) const;
  bool enqueuePacket(const uint8_t* data, size_t len, uint32_t seq, uint32_t timestampMs, uint8_t flags);
  bool sendQueuedPacket(WiFiUDP& udp, const String& host, uint16_t port);
  void recordUdpSend(bool ok, uint32_t durationUs);
  ScanHealth health() const;
  String healthJson() const;
  String matrixShapeJson() const;
  String matrixLayoutJson() const;
  uint32_t scanIntervalUs() const;
  uint32_t nextScanDueUs() const { return nextScanDueUs_; }

 private:
  struct SensorFilterState {
    bool initialized = false;
    float lowpass = 0.0f;
    uint8_t medianCount = 0;
    uint8_t medianCursor = 0;
    float medianValues[kFilterMedianMax] = {0};
  };

  bool sampleRawFrame(float* outValues, size_t count);
  bool sampleRawCell(uint16_t sensorIndex, float& outValue);
  void resetFilterState();
  float applyFilter(float value, uint16_t sensorIndex);
  void configurePins();
  void setAllColsInactive();
  void clearPacketQueue();
  void scheduleNextScan(uint32_t scanStartUs, uint32_t scanEndUs);
  void updateScanFps(uint32_t nowMs);
  void logPerformanceIfDue(uint32_t nowMs);

  struct PacketSlot {
    bool occupied = false;
    size_t len = 0;
    uint32_t seq = 0;
    uint32_t timestampMs = 0;
    uint8_t flags = 0;
    uint8_t bytes[kMaxPacketBytes] = {0};
  };

  volatile bool running_ = false;
  uint8_t rows_[kRows] = {0};
  uint8_t cols_[kCols] = {0};
  size_t rowCount_ = 0;
  size_t colCount_ = 0;
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
  bool queueEnabled_ = false;
  uint8_t queueDepthFrames_ = 0;
  uint8_t queueHead_ = 0;
  uint8_t queueTail_ = 0;
  uint8_t queueCount_ = 0;
  uint32_t queueDroppedFrames_ = 0;
  uint8_t queueMaxOccupiedFrames_ = 0;
  PacketSlot packetQueue_[kMaxScanRingFrames];
  uint32_t lastPerfLogMs_ = 0;
  float captureTotalsScratch_[kMaxSensors] = {0};
  float captureSampleScratch_[kMaxSensors] = {0};
  FilterConfig filterConfig_;
  SensorFilterState filterStates_[kMaxSensors];
  Calibration* calibration_ = nullptr;
  bool streamRawAdc_ = false;
};

}  // namespace nhos
