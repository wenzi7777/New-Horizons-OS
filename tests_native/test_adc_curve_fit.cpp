// AdcCurveFit: IDF's ESP32-S3 curve-fitting calibration without its uint64
// overflow (the 4983 mV a saturated ADC2 row read on a v1.5.F).
#include <cstdio>

#include "AdcCurveFit.h"

namespace {
int failures = 0;
void expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

using nhos::adc_curve::Unit;

// adc_cali_curve_fitting.c's cali_raw_to_voltage(), transcribed with its
// uint64 arithmetic (wrap included) -- what analogReadMilliVolts() returns.
int32_t idfRawToMv(Unit unit, uint32_t digi, int raw) {
  static const uint64_t kCoeff[2][5][2] = {
      {{644403418269478ULL, 1000000000000000ULL}, {644334888647536ULL, 10000000000000000ULL},
       {1297891447611ULL, 10000000000000000ULL}, {70769718ULL, 1000000000000000ULL},
       {13515ULL, 1000000000000000ULL}},
      {{12247719764336924ULL, 10000000000000000ULL}, {755717904943462ULL, 10000000000000000ULL},
       {1478791187119ULL, 10000000000000000ULL}, {79672528ULL, 1000000000000000ULL},
       {15038ULL, 1000000000000000ULL}},
  };
  static const int32_t kSign[2][5] = {{-1, -1, 1, -1, 1}, {1, -1, 1, -1, 1}};
  const int u = unit == Unit::Adc1 ? 0 : 1;
  const uint32_t coeffA = 65536u * 850u / digi;
  const uint64_t v = static_cast<uint64_t>(raw) * coeffA / 65536u;
  if (v == 0) {
    return 0;
  }
  uint64_t variable = 1;
  int32_t error = static_cast<int32_t>(variable * kCoeff[u][0][0] / kCoeff[u][0][1]) * kSign[u][0];
  for (int i = 1; i < 5; ++i) {
    variable *= v;
    const uint64_t term = variable * kCoeff[u][i][0] / kCoeff[u][i][1];  // may wrap
    error += static_cast<int32_t>(term) * kSign[u][i];
  }
  return static_cast<int32_t>(v) - error;
}

bool overflowsInIdf(Unit unit, uint32_t digi, int raw) {
  const uint64_t v = nhos::adc_curve::firstStepMv(digi, raw);
  const uint64_t coeff2 = unit == Unit::Adc1 ? 1297891447611ULL : 1478791187119ULL;
  return v * v > UINT64_MAX / coeff2;
}
}  // namespace

int main() {
  using nhos::adc_curve::rawToMv;

  // The field case: a v1.5.F whose ADC2 digi is ~985. IDF's reading jumps
  // from ~3123 to ~4967 at the last codes; ours stays on the curve.
  expect(idfRawToMv(Unit::Adc2, 985, 4093) == 3123, "IDF below the overflow");
  expect(idfRawToMv(Unit::Adc2, 985, 4095) > 4900, "IDF overflows at code 4095 (the 4983 mV symptom)");
  expect(rawToMv(Unit::Adc2, 985, 4093) == 3123, "unchanged below the overflow");
  expect(rawToMv(Unit::Adc2, 985, 4095) >= 3123 && rawToMv(Unit::Adc2, 985, 4095) < 3200,
         "saturation reads ~3.1 V, not ~5 V");

  // Bit-identical to IDF wherever IDF does not overflow; bounded and
  // continuous where it does. digi spans every value the eFuse encoding
  // allows at 12 dB (ADC1: 900..1155, ADC2: that -63..+15).
  bool identical = true;
  bool bounded = true;
  bool continuous = true;
  bool someOverflow[2] = {false, false};
  for (int u = 0; u < 2; ++u) {
    const Unit unit = u == 0 ? Unit::Adc1 : Unit::Adc2;
    for (uint32_t digi = 830; digi <= 1175; ++digi) {
      int32_t previous = 0;
      for (int raw = 0; raw <= nhos::adc_curve::kMaxRaw; ++raw) {
        const int32_t mv = rawToMv(unit, digi, raw);
        if (overflowsInIdf(unit, digi, raw)) {
          someOverflow[u] = true;
        } else if (mv != idfRawToMv(unit, digi, raw)) {
          identical = false;
        }
        if (mv < 0 || mv > 3300) {
          bounded = false;
        }
        if (raw > 0 && (mv - previous > 8 || mv - previous < -8)) {
          continuous = false;
        }
        previous = mv;
      }
    }
  }
  expect(identical, "identical to IDF where IDF is right");
  expect(bounded, "never above 3.3 V or below 0");
  expect(continuous, "no jump between neighbouring codes");
  expect(someOverflow[0] && someOverflow[1], "the sweep reaches the overflow on both units");

  expect(rawToMv(Unit::Adc2, 0, 4095) == 0, "no calibration point -> 0");
  expect(rawToMv(Unit::Adc2, 985, 0) == 0, "code 0 -> 0 mV, as IDF");
  expect(rawToMv(Unit::Adc2, 985, 65535) == rawToMv(Unit::Adc2, 985, 4095), "out-of-range raw clamps to 4095");

  if (failures == 0) {
    std::printf("adc curve fit: ok\n");
  }
  return failures == 0 ? 0 : 1;
}
