#pragma once

#include <Arduino.h>

#include "BoardConfig.h"

namespace nhos {

static constexpr char kProductName[] = "New Horizons OS Arduino";
static constexpr char kProtocolName[] = "NHO/Arduino/1";
static constexpr char kHardwareModel[] = NHOS_BOARD_NAME;
static constexpr char kFirmwareVersion[] = "v0.11.0";

static constexpr uint16_t kRows = NHOS_BOARD_ROWS;
static constexpr uint16_t kCols = NHOS_BOARD_COLS;
static constexpr uint16_t kMaxSensors = kRows * kCols;
static constexpr uint8_t kImuSampleFloats = 7 + (NHOS_BOARD_HAS_MAG ? 3 : 0);

static constexpr uint16_t kUdpStreamPort = 13250;
static constexpr uint16_t kDiscoveryPort = 22346;
static constexpr uint16_t kControlPort = 22345;

static constexpr uint16_t kPacketMagic = 0xA55A;
static constexpr uint8_t kPacketVersion = 3;
static constexpr uint8_t kPacketFlagImu = 0x01;
static constexpr uint8_t kPacketFlagBattery = 0x02;
static constexpr uint8_t kPacketFlagMag = 0x04;
static constexpr uint8_t kPacketFlagRawAdc = 0x08;
static constexpr uint8_t kPacketFlagHmac = 0x40;
static constexpr uint8_t kPacketFlagHeartbeat = 0x80;
static constexpr size_t kPacketHeaderLen = 20;
static constexpr size_t kPacketHmacLen = 16;
static constexpr size_t kMaxPacketBytes =
    kPacketHeaderLen +
    (kMaxSensors * sizeof(float)) +
    (kMaxSensors * sizeof(float)) +  // optional raw ADC block (parallel to matrix levels)
    (kImuSampleFloats * sizeof(float)) +
    4 +
    kPacketHmacLen;
static constexpr uint32_t kHeartbeatIntervalMs = 5000;

static constexpr uint16_t kDefaultTargetFps = 60;
static constexpr uint16_t kMaxTargetFps = 90;
static constexpr uint16_t kDefaultSettleUs = 20;
static constexpr uint8_t kDefaultSendEveryNFrames = 1;
static constexpr uint8_t kStandardScanRingFrames = 3;
static constexpr uint8_t kExtendedScanRingFrames = 5;
static constexpr size_t kScanRingFrames = kStandardScanRingFrames;
static constexpr size_t kMaxScanRingFrames = kExtendedScanRingFrames;

static constexpr uint32_t kWifiReconnectMs = 10000;
static constexpr uint32_t kBootWifiSetupWindowMs = 3000;
static constexpr uint8_t kSafeModeBootFailures = 3;
static constexpr size_t kDefaultLogMaxBytes = 12 * 1024;
static constexpr size_t kExtendedLogMaxBytes = 24 * 1024;

static constexpr char kDefaultApSsidPrefix[] = "NewHorizonsOS";
static constexpr char kDefaultApPassword[] = "";
static constexpr char kSetupPortalDomain[] = "newhorizons.os";
static constexpr uint16_t kSetupPortalPort = 80;
static constexpr char kDefaultUpdateManifestUrl[] =
    NHOS_BOARD_DEFAULT_OTA_MANIFEST_URL;

enum class RunMode : uint8_t {
  Normal = 0,
  Maintenance = 1,
  SafeMaintenance = 2,
};

struct RuntimeConfig {
  uint16_t targetFps = kDefaultTargetFps;
  uint16_t settleUs = kDefaultSettleUs;
  uint8_t sendEveryNFrames = kDefaultSendEveryNFrames;
  bool imuEnabled = true;
  bool indicatorsEnabled = true;
};

}  // namespace nhos
