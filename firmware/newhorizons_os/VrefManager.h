#pragma once

#include <Arduino.h>

#include "VrefPolicy.h"

namespace nhos {

class Storage;

// v2.0.F only: owns the MCP4725 that sets Vref (0x60) and the PCA9536 whose
// IO0 switches the calibration network (0x41). It keeps one invariant: the
// DAC is at the safe 0.33 V -- and its EEPROM holds that value -- whenever
// the ESP32 could be starting up (see VrefPolicy.h for why). Boards without
// the parts compile this to an inert stub.
class VrefManager {
 public:
  // After Wire.begin() and before WiFi / the scanner: repairs the DAC's EEPROM
  // if it is not the safe code, drives the DAC to 0.33 V and opens the
  // calibration switch. `storage` holds the operator's Vref ceiling.
  void begin(Storage& storage);
  bool available() const { return dacOk_; }
  bool calibrationAvailable() const { return pcaOk_; }

  // Vref goes to the DAC register only, never to the EEPROM.
  bool setVrefMv(uint32_t mv, const char*& error);
  bool setCalibration(bool on, const char*& error);
  // Operator ceiling for set_vref; persisted, clamped to the policy range.
  void setLimitMv(uint32_t mv);
  uint16_t limitMv() const { return limitMv_; }
  uint16_t vrefMv() const { return vrefMv_; }
  bool calibrationOn() const { return calibrationOn_; }

  // Back to 0.33 V with the calibration switch open. Registered as an ESP-IDF
  // shutdown handler (every ESP.restart()) and called before soft-off.
  void restoreSafeState();

  String statusJson() const;
  static String i2cScanJson();

 private:
  bool readDac(Mcp4725Read& out);
  bool writeDac(uint16_t code, bool persist);
  bool setPcaOutput(bool on);
  static void onShutdown();

  Storage* storage_ = nullptr;
  bool dacOk_ = false;
  bool pcaOk_ = false;
  bool eepromWritten_ = false;
  bool eepromVerified_ = false;
  bool calibrationOn_ = false;
  uint16_t vrefMv_ = kVrefSafeMv;
  uint16_t limitMv_ = kVrefDefaultLimitMv;
  uint16_t bootEepromCode_ = 0;
  uint16_t eepromCode_ = 0;
  const char* lastError_ = "";
};

}  // namespace nhos
