#include "AppDeviceControl.h"

#include <math.h>
#include <string.h>

#include "ConfigRegistry.h"
#include "DeviceConfig.h"
#include "ImuManager.h"
#include "MatrixScanner.h"

namespace nhos {

DeviceConfig* AppDeviceControl::config_ = nullptr;
MatrixScanner* AppDeviceControl::scanner_ = nullptr;
ImuManager* AppDeviceControl::imu_ = nullptr;
const void* AppDeviceControl::owner_[kVmDeviceConfigKeyCount] = {};
float AppDeviceControl::value_[kVmDeviceConfigKeyCount] = {};
const void* AppDeviceControl::pausedBy_ = nullptr;

namespace {

enum class Group : uint8_t { Timing, Filter, Imu, None };

Group groupOf(const char* key) {
  if (strncmp(key, "scan.", 5) == 0) return Group::Timing;
  if (strncmp(key, "filter.", 7) == 0) return Group::Filter;
  if (strcmp(key, "imu.enabled") == 0) return Group::Imu;
  return Group::None;
}

}  // namespace

void AppDeviceControl::begin(DeviceConfig* config, MatrixScanner* scanner, ImuManager* imu) {
  config_ = config;
  scanner_ = scanner;
  imu_ = imu;
}

int8_t AppDeviceControl::indexOf(const char* key) {
  if (key == nullptr) return -1;
  for (uint8_t i = 0; i < kVmDeviceConfigKeyCount; ++i) {
    if (strcmp(key, kVmDeviceConfigKeys[i]) == 0) return static_cast<int8_t>(i);
  }
  return -1;
}

float AppDeviceControl::get(const char* key) {
  const int8_t index = indexOf(key);
  if (index < 0) return 0.0f;
  if (owner_[index] != nullptr) return value_[index];
  if (config_ == nullptr) return 0.0f;
  const DeviceConfigData& data = config_->data();
  if (strcmp(key, "scan.target_fps") == 0) return data.scanTiming.targetFps;
  if (strcmp(key, "scan.send_every_n_frames") == 0) return data.scanTiming.sendEveryNFrames;
  if (strcmp(key, "filter.enabled") == 0) return data.filter.enabled ? 1.0f : 0.0f;
  if (strcmp(key, "filter.median") == 0) return data.filter.median;
  if (strcmp(key, "filter.alpha") == 0) return data.filter.alpha;
  if (strcmp(key, "imu.enabled") == 0) return data.imuEnabled ? 1.0f : 0.0f;
  return 0.0f;
}

bool AppDeviceControl::set(const void* owner, const char* key, float value) {
  const int8_t index = indexOf(key);
  if (index < 0 || owner == nullptr || config_ == nullptr || isnan(value)) return false;
  // The same ranges config_set publishes (ConfigRegistry), applied here as a
  // gate: an app gets no wider a range than the operator.
  const ConfigEntry* entry = ConfigRegistry::find(String(key));
  if (entry == nullptr) return false;
  float lo = entry->min;
  float hi = entry->max;
  bool integer = entry->type != ConfigType::Float;
  if (entry->type == ConfigType::Bool) {
    lo = 0.0f;
    hi = 1.0f;
  }
  if (!(value >= lo && value <= hi) || (integer && value != floorf(value))) return false;
  owner_[index] = owner;
  value_[index] = value;
  apply();
  return true;
}

void AppDeviceControl::setStream(const void* owner, bool on) {
  if (owner == nullptr) return;
  if (!on) {
    pausedBy_ = owner;
  } else if (pausedBy_ != nullptr) {
    // Any app may resume what an app paused; the operator's own stop
    // (stream_stop) is a different switch and is not touched here.
    pausedBy_ = nullptr;
  }
}

void AppDeviceControl::release(const void* owner) {
  if (owner == nullptr) return;
  bool changed = false;
  for (uint8_t i = 0; i < kVmDeviceConfigKeyCount; ++i) {
    if (owner_[i] == owner) {
      owner_[i] = nullptr;
      changed = true;
    }
  }
  if (pausedBy_ == owner) pausedBy_ = nullptr;
  if (changed) apply();
}

void AppDeviceControl::apply() {
  if (config_ == nullptr) return;
  const DeviceConfigData& data = config_->data();
  if (scanner_ != nullptr) {
    const uint16_t fps = static_cast<uint16_t>(get("scan.target_fps"));
    const uint16_t every = static_cast<uint16_t>(get("scan.send_every_n_frames"));
    if (fps != scanner_->targetFps() || every != scanner_->sendEveryNFrames()) {
      scanner_->setTiming(fps, data.scanTiming.settleUs, every);
      if (imu_ != nullptr) imu_->setServiceIntervalUs(scanner_->scanIntervalUs());
    }
    FilterConfig filter = data.filter;
    filter.enabled = get("filter.enabled") != 0.0f;
    filter.median = static_cast<uint8_t>(get("filter.median"));
    filter.alpha = get("filter.alpha");
    scanner_->setFilterConfig(filter);
  }
  if (imu_ != nullptr) {
    const bool want = get("imu.enabled") != 0.0f;
    if (want != imu_->enabled()) imu_->setEnabled(want);
  }
}

}  // namespace nhos
