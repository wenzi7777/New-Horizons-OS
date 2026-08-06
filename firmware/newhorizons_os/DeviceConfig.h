#pragma once

#include <Arduino.h>

#include "Config.h"
#include "Storage.h"

namespace nhos {

struct MatrixLayoutConfig {
  uint8_t analogPins[kRows] = {0};
  uint8_t selectPins[kCols] = {0};
  size_t analogCount = 0;
  size_t selectCount = 0;
};

struct ScanTimingConfig {
  uint16_t targetFps = kDefaultTargetFps;
  uint16_t settleUs = kDefaultSettleUs;
  uint16_t sendEveryNFrames = kDefaultSendEveryNFrames;
};

struct StreamBufferConfig {
  bool enabled = true;
  String mode = "standard";
  uint8_t depthFrames = 3;
};

constexpr uint8_t kFilterMedianMax = 5;
constexpr uint8_t kFilterDefaultMedian = 3;
constexpr float kFilterDefaultAlpha = 0.25f;
constexpr float kFilterAlphaMin = 0.05f;
constexpr float kFilterAlphaMax = 0.6f;

struct FilterConfig {
  bool enabled = false;
  uint8_t median = kFilterDefaultMedian;
  float alpha = kFilterDefaultAlpha;
};

struct ExternalLedConfig {
  String mode = "off";
  String preset = "system_status";
  float brightness = 0.35f;
  String color = "teal";
};

struct OledConfig {
  String mode = "off";
  String page = "live_status";
  uint8_t updateHz = 1;
  uint8_t contrast = 128;
  uint8_t rotation = 0;
};

struct LogConfig {
  bool enabled = true;
  String level = "info";
  String mode = "standard";
  size_t maxBytes = kDefaultLogMaxBytes;
};

struct OtaConfig {
  bool autoApplyOnBoot = true;
  String manifestUrl = kDefaultUpdateManifestUrl;
};

// New Horizons Direct: which transport StreamTransport should use.
// Defaults to "wifi_udp" so existing lab devices are unaffected unless
// explicitly switched (via the `set_transport` command or local config
// edit). `hubMac` is only meaningful in "espnow" mode ("AA:BB:CC:DD:EE:FF"
// format). `lastKnownChannel` is a fast-reconnect hint for EspNowPairing's
// channel scan (0 = unknown, scan from scratch) -- the Hub's own WiFi-STA
// channel can change between boots, so this is a hint, not a guarantee.
struct TransportConfig {
  String mode = "wifi_udp";
  String hubMac = "";
  uint8_t lastKnownChannel = 0;
};

struct DeviceConfigData {
  uint8_t schemaVersion = 1;
  MatrixLayoutConfig matrixLayout;
  ScanTimingConfig scanTiming;
  StreamBufferConfig streamBuffer;
  FilterConfig filter;
  bool imuEnabled = true;
  bool streamRawAdc = false;
  LogConfig logging;
  OtaConfig ota;
  ExternalLedConfig externalLed;
  OledConfig oled;
  TransportConfig transport;
};

class DeviceConfig {
 public:
  bool load(Storage& storage);
  bool save(Storage& storage);

  const DeviceConfigData& data() const;
  DeviceConfigData& mutableData();

  bool setMatrixLayout(const uint8_t* analogPins, size_t analogCount, const uint8_t* selectPins, size_t selectCount);
  bool setScanTiming(uint16_t targetFps, uint16_t settleUs, uint16_t sendEveryNFrames);
  bool setStreamBuffer(bool enabled, const String& mode);
  bool setFilter(bool enabled, uint8_t median, float alpha);
  bool setImuEnabled(bool enabled);
  bool setStreamRawAdc(bool enabled);
  bool setLogging(bool enabled, const String& level, const String& mode, size_t maxBytes);
  bool setOtaConfig(bool autoApplyOnBoot, const String& manifestUrl);
  bool setExternalLed(const String& mode, const String& preset, float brightness, const String& color);
  bool setOled(const String& mode, const String& page, uint8_t updateHz, uint8_t contrast, uint8_t rotation);
  bool setTransport(const String& mode, const String& hubMac);
  void setTransportLastKnownChannel(uint8_t channel);

  String statusJson() const;
  String filterJson() const;
  String loggingJson() const;
  String otaJson() const;
  String streamBufferJson() const;
  String transportJson() const;
  String configJson() const;

  static bool validLogLevel(const String& level);
  static bool validLogMode(const String& mode);
  static bool validStreamBufferMode(const String& mode);
  static bool validExternalLedMode(const String& mode);
  static String canonicalExternalLedPreset(const String& preset);
  static bool validOledMode(const String& mode);
  static bool validTransportMode(const String& mode);
  static bool validHubMac(const String& mac);

 private:
  void setDefaults();
  bool applyJson(const String& json);
  String toJson() const;
  String lastErrorJson() const;

  DeviceConfigData data_;
  bool loaded_ = false;
  String lastError_;
};

}  // namespace nhos
