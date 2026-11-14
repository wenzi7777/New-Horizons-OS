#pragma once

#include <Arduino.h>

#include "AppContract.h"

namespace nhos {

class DeviceConfig;
class ImuManager;
class MatrixScanner;

// What device.configGet/configSet and device.stream (device_config /
// device_stream, v2.1.0) may change, and the undo when the app stops.
//
// Only the settings in kVmDeviceConfigKeys, checked against the ranges
// ConfigRegistry publishes, and never saved: a value an app sets is applied
// to the running scanner / IMU and nothing else, so DeviceConfig keeps the
// operator's value and a reboot -- or the app stopping, faulting or being
// removed -- brings it back. Two apps setting the same key: the later one
// holds it, and the earlier one's stop leaves it alone.
//
// One instance for the device (static state), keyed by the AppVm asking.
class AppDeviceControl {
 public:
  static void begin(DeviceConfig* config, MatrixScanner* scanner, ImuManager* imu);

  static float get(const char* key);
  // False when the key is not offered or the value is out of range.
  static bool set(const void* owner, const char* key, float value);
  static void setStream(const void* owner, bool on);
  // Read by the stream path: true while an app has paused streaming.
  static bool streamPaused() { return pausedBy_ != nullptr; }
  // Undoes everything `owner` changed.
  static void release(const void* owner);

 private:
  static int8_t indexOf(const char* key);
  static void apply();

  static DeviceConfig* config_;
  static MatrixScanner* scanner_;
  static ImuManager* imu_;
  static const void* owner_[kVmDeviceConfigKeyCount];
  static float value_[kVmDeviceConfigKeyCount];
  static const void* pausedBy_;
};

}  // namespace nhos
