#include "BatteryIdReader.h"

#include <algorithm>

#if defined(ESP_ARDUINO_VERSION_MAJOR)
#include <esp_efuse_rtc_calib.h>
#include <hal/adc_types.h>
#endif

#include "AdcCurveFit.h"

namespace nhos {
namespace {

constexpr uint8_t kSamples = 9;

uint32_t adc2CalibrationDigi() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && defined(CONFIG_IDF_TARGET_ESP32S3)
  const int version = esp_efuse_rtc_calib_get_ver();
  if (version < ESP_EFUSE_ADC_CALIB_VER_MIN || version > ESP_EFUSE_ADC_CALIB_VER_MAX) {
    return 0;
  }
  uint32_t digi = 0;
  uint32_t mv = 0;
  if (esp_efuse_rtc_calib_get_cal_voltage(version, ADC_UNIT_2, ADC_ATTEN_DB_12, &digi, &mv) == ESP_OK &&
      mv == adc_curve::kCalVoltageMv) {
    return digi;
  }
#endif
  return 0;
}

}  // namespace

bool readBatteryIdMv(uint8_t pin, uint16_t& mv) {
  const uint32_t digi = adc2CalibrationDigi();
  uint16_t values[kSamples] = {};
  uint8_t good = 0;
  for (uint8_t i = 0; i < kSamples; ++i) {
    const uint16_t raw = analogRead(pin);
    if (raw <= adc_curve::kMaxRaw) {
      values[good++] = raw;
    }
    delay(2);
  }
  if (good < 3) {
    return false;
  }
  std::sort(values, values + good);
  const uint16_t median = values[good / 2];
  if (digi == 0) {
    // No eFuse calibration point: let the core convert it.
    mv = static_cast<uint16_t>(analogReadMilliVolts(pin));
    return true;
  }
  const int32_t converted = adc_curve::rawToMv(adc_curve::Unit::Adc2, digi, median);
  mv = static_cast<uint16_t>(converted < 0 ? 0 : (converted > 65535 ? 65535 : converted));
  return true;
}

}  // namespace nhos
