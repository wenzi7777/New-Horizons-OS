#pragma once

#include <cstdint>

// Raw ADC code -> millivolts for the matrix rows, kept free of Arduino so the
// host-native tests can pin it.
//
// This is ESP-IDF's ESP32-S3 curve-fitting calibration (esp_adc
// adc_cali_curve_fitting.c, the scheme analogReadMilliVolts() uses), at the
// 12 dB attenuation and 12-bit width the scanner reads with, minus one bug:
// IDF evaluates each error term as uint64 var * coeff / divisor, and the
// x^2 term overflows once the first-step voltage passes
//   ADC2: sqrt(2^64 / 1478791187119) = 3532 mV
//   ADC1: sqrt(2^64 / 1297891447611) = 3770 mV
// The term then wraps to ~0, the ~1840 mV it should subtract is lost, and a
// saturated row reads ~4970-4983 mV instead of ~3100 (seen on a v1.5.F's
// GPIO13/14, recording 120922). Whether a chip gets there depends on its
// eFuse calibration point: first-step mV at code 4095 = 4095 * 850 / digi,
// so ADC2 overflows on every chip whose digi is below 986 and ADC1 below 924.
// Still unfixed in IDF v6.0.1.
//
// Every term that fits in uint64 is computed exactly as IDF does, so a
// reading that IDF got right comes out bit-identical (stored tares and
// calibration levels stay valid); only the overflowing terms are evaluated
// in double instead.

namespace nhos {
namespace adc_curve {

// The calibration point IDF reads from eFuse: digi = the code the chip read
// for kCalVoltageMv at this attenuation (esp_efuse_rtc_calib_get_cal_voltage).
static constexpr uint32_t kCalVoltageMv = 850;
static constexpr int kMaxRaw = 4095;

enum class Unit : uint8_t { Adc1 = 0, Adc2 = 1 };

// The first step alone (raw * 850 / digi), as IDF computes it.
uint32_t firstStepMv(uint32_t calDigi, int raw);

// The calibrated reading, in mV. 0 for raw <= 0 or calDigi == 0; raw above
// kMaxRaw is clamped to it (a 12-bit read never returns more).
int32_t rawToMv(Unit unit, uint32_t calDigi, int raw);

}  // namespace adc_curve
}  // namespace nhos
