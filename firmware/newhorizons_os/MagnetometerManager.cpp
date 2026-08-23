#include "MagnetometerManager.h"
#include <Wire.h>
#include "BoardConfig.h"
#if NHOS_BOARD_MAG_MODEL == 1
#include "Arduino_BMI270_BMM150.h"
#endif
namespace nhos {
namespace { constexpr uint8_t kBmm350Address = 0x14; constexpr uint8_t kBmm350ChipId = 0x00; constexpr uint8_t kBmm350PmuCmd = 0x06; constexpr uint8_t kBmm350NormalMode = 0x02; constexpr uint8_t kBmm350DataX = 0x31; }
void MagnetometerManager::begin() { ready_ = false; valid_ = false; error_ = "not_initialized"; lastPollMs_ = 0;
#if NHOS_BOARD_MAG_MODEL == 2
  Wire.beginTransmission(kBmm350Address); Wire.write(kBmm350ChipId); if (Wire.endTransmission(false) || Wire.requestFrom(kBmm350Address, static_cast<uint8_t>(1)) != 1 || !Wire.available() || Wire.read() != 0x33) { error_ = "bmm350_not_detected"; return; } Wire.beginTransmission(kBmm350Address); Wire.write(kBmm350PmuCmd); Wire.write(kBmm350NormalMode); if (Wire.endTransmission() != 0) { error_ = "bmm350_normal_mode_failed"; return; } ready_ = true; error_ = "";
#elif NHOS_BOARD_MAG_MODEL == 1
  ready_ = true; error_ = "";
#else
  error_ = "magnetometer_not_supported";
#endif
}
void MagnetometerManager::service(uint32_t nowMs) { if (!ready_ || (lastPollMs_ && nowMs - lastPollMs_ < 50)) return; lastPollMs_ = nowMs; float next[3];
#if NHOS_BOARD_MAG_MODEL == 1
  if (!IMU.readMagneticField(next[0], next[1], next[2])) { valid_ = false; error_ = "bmm150_read_failed"; return; }
#elif NHOS_BOARD_MAG_MODEL == 2
  if (!readBmm350(next)) { valid_ = false; error_ = "bmm350_read_failed"; return; }
#else
  return;
#endif
  sample_[0] = next[0]; sample_[1] = next[1]; sample_[2] = next[2]; valid_ = true; error_ = ""; }
bool MagnetometerManager::readBmm350(float out[3]) { Wire.beginTransmission(kBmm350Address); Wire.write(kBmm350DataX); if (Wire.endTransmission(false) != 0 || Wire.requestFrom(kBmm350Address, static_cast<uint8_t>(9)) != 9) return false; uint8_t raw[9]; for (uint8_t i = 0; i < 9; ++i) { if (!Wire.available()) return false; raw[i] = Wire.read(); } for (uint8_t i = 0; i < 3; ++i) { int32_t value = static_cast<int32_t>(raw[i * 3]) | (static_cast<int32_t>(raw[i * 3 + 1]) << 8) | (static_cast<int32_t>(raw[i * 3 + 2]) << 16); if (value & 0x800000) value |= ~0xFFFFFF; out[i] = static_cast<float>(value); } return true; }
bool MagnetometerManager::copyLatestSample(float out[3]) const { if (!out || !valid_) return false; out[0] = sample_[0]; out[1] = sample_[1]; out[2] = sample_[2]; return true; }
String MagnetometerManager::statusJson() const { String out = "{\"model\":\"";
#if NHOS_BOARD_MAG_MODEL == 2
 out += "BMM350";
#elif NHOS_BOARD_MAG_MODEL == 1
 out += "BMM150";
#else
 out += "none";
#endif
 out += "\",\"ready\":"; out += ready_ ? "true" : "false"; out += ",\"sample_valid\":"; out += valid_ ? "true" : "false"; out += ",\"last_error\":\"" + error_ + "\"}"; return out; }
}  // namespace nhos
