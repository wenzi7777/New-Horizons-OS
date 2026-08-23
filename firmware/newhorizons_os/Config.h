#pragma once

#include <Arduino.h>

#include "BoardConfig.h"
#include "PacketWire.h"

#ifndef NHOS_FIRMWARE_VERSION
#define NHOS_FIRMWARE_VERSION "v0.17.1"
#endif

namespace nhos {

static constexpr char kProductName[] = "New Horizons OS Arduino";
static constexpr char kProtocolName[] = "NHO/Arduino/1";
static constexpr char kHardwareModel[] = NHOS_BOARD_NAME;
static constexpr char kFirmwareVersion[] = NHOS_FIRMWARE_VERSION;

static constexpr uint16_t kRows = NHOS_BOARD_ROWS;
static constexpr uint16_t kCols = NHOS_BOARD_COLS;
static constexpr uint16_t kMaxSensors = kRows * kCols;
// Magnetometer samples are supplied as a separately optional three-float
// block.  This keeps a seven-float IMU buffer from being mistaken for MAG.
static constexpr uint8_t kImuSampleFloats = 7;

static constexpr uint16_t kUdpStreamPort = 13250;
static constexpr uint16_t kDiscoveryPort = 22346;
static constexpr uint16_t kControlPort = 22345;

static constexpr size_t kMaxPacketBytes =
    kPacketHeaderLen +
    (kMaxSensors * sizeof(float)) +
    (kMaxSensors * sizeof(float)) +  // optional raw ADC block (parallel to matrix levels)
    kPacketBaseImuBytes +
    kPacketMagBytes +
    kPacketBatteryBytes +
    kPacketExtensionBudget +
    kPacketHmacLen;
static_assert(kMaxPacketBytes <= kEspNowDataFrameBudget,
              "v5 packet must stay within the ESP-NOW data-frame budget");
static constexpr uint32_t kHeartbeatIntervalMs = 5000;
static constexpr uint32_t kTimeSyncValidEpochS = 1600000000;  // ~2020-09-13; time() below this means "not yet synced"

static constexpr uint16_t kDefaultTargetFps = 60;
static constexpr uint16_t kMaxTargetFps = 120;
static constexpr uint16_t kDefaultSettleUs = 20;
static constexpr uint8_t kDefaultSendEveryNFrames = 1;
static constexpr uint8_t kStandardScanRingFrames = 3;
static constexpr uint8_t kExtendedScanRingFrames = 5;
static constexpr size_t kScanRingFrames = kStandardScanRingFrames;
static constexpr size_t kMaxScanRingFrames = kExtendedScanRingFrames;

static constexpr uint32_t kWifiReconnectMs = 10000;
// If the ongoing background reconnect (WifiManager::service(), after a
// previously-successful connection drops) can't get back on the air for
// this long, give up and reopen the setup portal rather than retrying
// silently forever -- mirrors the boot-time "one failed connect attempt
// opens the portal immediately" behavior, just with a grace period since
// this path also covers routine transient outages (AP reboot, roaming)
// that shouldn't bounce the device into setup mode. Starting guess, same
// order of magnitude as kEspNowPairingTimeoutMs for consistency -- not a
// tuned promise.
static constexpr uint32_t kWifiReconnectFallbackMs = 120000;
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
