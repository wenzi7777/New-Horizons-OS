#include "VrefManager.h"

#include <Wire.h>
#include <esp_system.h>

#include "BoardConfig.h"
#include "JsonUtils.h"
#include "Storage.h"

namespace nhos {
namespace {

constexpr uint8_t kMcp4725Address = 0x60;
constexpr uint8_t kPca9536Address = 0x41;
constexpr const char* kLimitKey = "vref_lim_mv";
// MCP4725 EEPROM write: 25 ms typical, 50 ms maximum.
constexpr uint32_t kEepromWriteTimeoutMs = 120;

VrefManager* shutdownInstance = nullptr;

}  // namespace

#if NHOS_BOARD_HAS_DAC_VREF

bool VrefManager::readDac(Mcp4725Read& out) {
  uint8_t buf[5] = {};
  const uint8_t got = Wire.requestFrom(kMcp4725Address, static_cast<uint8_t>(5));
  if (got != 5) {
    while (Wire.available()) {
      Wire.read();
    }
    lastError_ = "dac_read_failed";
    return false;
  }
  for (uint8_t i = 0; i < 5; ++i) {
    buf[i] = static_cast<uint8_t>(Wire.read());
  }
  return decodeMcp4725Read(buf, sizeof(buf), out);
}

bool VrefManager::writeDac(uint16_t code, bool persist) {
  uint8_t bytes[3] = {};
  const size_t len = encodeMcp4725Write(code, persist, bytes);
  Wire.beginTransmission(kMcp4725Address);
  Wire.write(bytes, len);
  if (Wire.endTransmission(true) != 0) {
    lastError_ = "dac_write_failed";
    return false;
  }
  return true;
}

bool VrefManager::setPcaOutput(bool on) {
  Wire.beginTransmission(kPca9536Address);
  Wire.write(kPca9536OutputReg);
  Wire.write(pca9536OutputByte(on));
  if (Wire.endTransmission(true) != 0) {
    lastError_ = "pca9536_write_failed";
    return false;
  }
  return true;
}

void VrefManager::begin(Storage& storage) {
  storage_ = &storage;
  limitMv_ = clampVrefLimitMv(storage.getUInt(kLimitKey, kVrefDefaultLimitMv));

  // 1. DAC: the EEPROM is what the chip comes up with after any reset, so it
  //    must be the safe code. Compare to the target (not to the factory
  //    1.65 V) and write only on a mismatch -- a correct board costs no wear.
  Mcp4725Read state;
  if (readDac(state)) {
    dacOk_ = true;
    bootEepromCode_ = state.eepromCode;
    eepromCode_ = state.eepromCode;
    const uint16_t safeCode = vrefMvToDacCode(kVrefSafeMv);
    if (dacEepromNeedsWrite(state.eepromCode)) {
      if (writeDac(safeCode, true)) {
        const uint32_t startMs = millis();
        Mcp4725Read after;
        bool done = false;
        while (millis() - startMs < kEepromWriteTimeoutMs) {
          delay(5);
          if (readDac(after) && after.ready) {
            done = true;
            break;
          }
        }
        eepromWritten_ = true;
        if (done) {
          eepromCode_ = after.eepromCode;
          eepromVerified_ = !dacEepromNeedsWrite(after.eepromCode);
        }
        if (!eepromVerified_) {
          lastError_ = "dac_eeprom_verify_failed";
        }
      }
    } else {
      eepromVerified_ = true;
    }
    // 2. The register may hold anything (a soft reboot keeps it): safe value.
    if (writeDac(vrefMvToDacCode(kVrefSafeMv), false)) {
      vrefMv_ = kVrefSafeMv;
    }
  } else {
    dacOk_ = false;
  }

  // 3. Calibration switch. The PCA9536 output register powers up high, so
  //    write it low first, then turn IO0 into an output.
  if (setPcaOutput(false)) {
    Wire.beginTransmission(kPca9536Address);
    Wire.write(kPca9536ConfigReg);
    Wire.write(pca9536ConfigByte());
    pcaOk_ = Wire.endTransmission(true) == 0;
    if (!pcaOk_) {
      lastError_ = "pca9536_config_failed";
    }
  }
  calibrationOn_ = false;

  shutdownInstance = this;
  esp_register_shutdown_handler(&VrefManager::onShutdown);
}

bool VrefManager::setVrefMv(uint32_t mv, const char*& error) {
  if (!dacOk_) {
    error = "vref_unavailable";
    return false;
  }
  switch (checkVrefRequest(mv, limitMv_)) {
    case VrefRequest::BelowMinimum:
      error = "vref_below_minimum";
      return false;
    case VrefRequest::AboveLimit:
      error = "vref_above_limit";
      return false;
    case VrefRequest::Ok:
      break;
  }
  if (!writeDac(vrefMvToDacCode(static_cast<uint16_t>(mv)), false)) {
    error = lastError_;
    return false;
  }
  vrefMv_ = dacCodeToVrefMv(vrefMvToDacCode(static_cast<uint16_t>(mv)));
  return true;
}

bool VrefManager::setCalibration(bool on, const char*& error) {
  if (!pcaOk_) {
    error = "calibration_switch_unavailable";
    return false;
  }
  if (!setPcaOutput(on)) {
    error = lastError_;
    return false;
  }
  calibrationOn_ = on;
  return true;
}

void VrefManager::setLimitMv(uint32_t mv) {
  limitMv_ = clampVrefLimitMv(mv);
  if (storage_ != nullptr) {
    storage_->putUInt(kLimitKey, limitMv_);
  }
  // Lowering the ceiling below the running Vref pulls it down at once.
  if (dacOk_ && vrefMv_ > limitMv_) {
    const uint16_t code = vrefMvToDacCode(limitMv_);
    if (writeDac(code, false)) {
      vrefMv_ = dacCodeToVrefMv(code);
    }
  }
}

void VrefManager::restoreSafeState() {
  if (dacOk_ && vrefMv_ != kVrefSafeMv) {
    if (writeDac(vrefMvToDacCode(kVrefSafeMv), false)) {
      vrefMv_ = kVrefSafeMv;
    }
  }
  if (pcaOk_ && calibrationOn_) {
    if (setPcaOutput(false)) {
      calibrationOn_ = false;
    }
  }
}

void VrefManager::onShutdown() {
  if (shutdownInstance != nullptr) {
    shutdownInstance->restoreSafeState();
  }
}

String VrefManager::statusJson() const {
  String out = "{";
  bool first = true;
  jsonBoolField(out, "supported", true, first);
  jsonBoolField(out, "ok", dacOk_, first);
  jsonUnsignedField(out, "mv", vrefMv_, first);
  jsonUnsignedField(out, "limit_mv", limitMv_, first);
  jsonUnsignedField(out, "safe_mv", kVrefSafeMv, first);
  jsonUnsignedField(out, "eeprom_mv", dacCodeToVrefMv(eepromCode_), first);
  jsonBoolField(out, "eeprom_written_at_boot", eepromWritten_, first);
  jsonBoolField(out, "eeprom_ok", eepromVerified_, first);
  jsonUnsignedField(out, "boot_eeprom_mv", dacCodeToVrefMv(bootEepromCode_), first);
  jsonBoolField(out, "calibration_available", pcaOk_, first);
  jsonBoolField(out, "calibration_on", calibrationOn_, first);
  if (lastError_[0] != '\0') {
    jsonStringField(out, "last_error", lastError_, first);
  }
  out += "}";
  return out;
}

#else  // !NHOS_BOARD_HAS_DAC_VREF

bool VrefManager::readDac(Mcp4725Read&) { return false; }
bool VrefManager::writeDac(uint16_t, bool) { return false; }
bool VrefManager::setPcaOutput(bool) { return false; }
void VrefManager::begin(Storage&) {}
bool VrefManager::setVrefMv(uint32_t, const char*& error) {
  error = "vref_unsupported";
  return false;
}
bool VrefManager::setCalibration(bool, const char*& error) {
  error = "calibration_switch_unsupported";
  return false;
}
void VrefManager::setLimitMv(uint32_t) {}
void VrefManager::restoreSafeState() {}
void VrefManager::onShutdown() {}
String VrefManager::statusJson() const { return "{\"supported\":false}"; }

#endif

// A plain probe of every address the boards use; works on all boards so a
// freshly assembled one can be checked without extra tooling.
String VrefManager::i2cScanJson() {
  static const uint8_t kKnown[] = {0x14, 0x36, 0x3C, 0x3D, 0x41, 0x60, 0x68, 0x6A};
  static const char kHex[] = "0123456789abcdef";
  String out = "[";
  bool first = true;
  for (uint8_t address : kKnown) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission(true) == 0) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += "\"0x";
      out += kHex[address >> 4];
      out += kHex[address & 0x0F];
      out += "\"";
    }
  }
  out += "]";
  return out;
}

}  // namespace nhos
