#pragma once

#include <Arduino.h>

#include "BoardConfig.h"
#include "PacketWire.h"

#ifndef NHOS_FIRMWARE_VERSION
#define NHOS_FIRMWARE_VERSION "v1.17.0"
#endif

namespace nhos {

static constexpr char kProductName[] = "NHOS Arduino";
static constexpr char kProtocolName[] = "NHO/Arduino/1";
static constexpr char kHardwareModel[] = NHOS_BOARD_NAME;
// The pre-rename name OTA manifests still carry (see BoardConfig.h).
static constexpr char kOtaManifestModel[] = NHOS_BOARD_OTA_MODEL;
static constexpr char kFirmwareVersion[] = NHOS_FIRMWARE_VERSION;

static constexpr uint16_t kRows = NHOS_BOARD_ROWS;
static constexpr uint16_t kCols = NHOS_BOARD_COLS;
static constexpr uint16_t kMaxSensors = kRows * kCols;
// Magnetometer samples are supplied as a separately optional three-float
// block.  This keeps a seven-float IMU buffer from being mistaken for MAG.
static constexpr uint8_t kImuSampleFloats = 7;

// kUdpStreamPort / kDiscoveryPort / kControlPort: PacketWireConstants.h.

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
// Clock service (ClockService.h). The NTP interval is far below ESP-IDF's
// 3 h default: a ±20 ppm crystal drifts ~200 ms in 3 h but ~12 ms in 10 min.
static constexpr char kDefaultNtpServer[] = "pool.ntp.org";
static constexpr char kFallbackNtpServer[] = "time.google.com";
static constexpr size_t kMaxNtpServerLen = 63;
static constexpr uint32_t kDefaultNtpIntervalS = 600;
static constexpr uint32_t kMinNtpIntervalS = 60;
static constexpr uint32_t kMaxNtpIntervalS = 86400;
// Time exchanges with the Gateway (WiFi mode) or Hub (ESP-NOW mode).
static constexpr uint16_t kDefaultSyncIntervalS = 15;
static constexpr uint16_t kMinSyncIntervalS = 5;
static constexpr uint16_t kMaxSyncIntervalS = 3600;

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

static constexpr char kDefaultApSsidPrefix[] = "NHOS";
static constexpr char kDefaultApPassword[] = "";
static constexpr char kSetupPortalDomain[] = "nhos.os";
static constexpr uint16_t kSetupPortalPort = 80;
static constexpr char kUpdateManifestBase[] = NHOS_BOARD_OTA_MANIFEST_BASE;
// The stable manifest, which every device followed before update channels
// existed. A stored manifest_url equal to it is migrated to "follow the
// channel" (DeviceConfig::setOtaConfig).
static constexpr char kDefaultUpdateManifestUrl[] = NHOS_BOARD_OTA_MANIFEST_BASE "-latest.json";

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
