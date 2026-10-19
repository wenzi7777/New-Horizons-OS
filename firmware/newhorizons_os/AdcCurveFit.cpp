#include "AdcCurveFit.h"

#include <cmath>
#include <limits>

namespace nhos {
namespace adc_curve {

namespace {

struct Term {
  uint64_t coeff;
  uint64_t divisor;
  int32_t sign;
};

constexpr int kTermCount = 5;
constexpr uint32_t kCoeffAScaling = 65536;

// esp_adc/esp32s3/curve_fitting_coefficients.c, atten3 rows (IDF v5.5.1 and
// v6.0.1 carry the same values).
constexpr Term kAdc1Atten3[kTermCount] = {
    {644403418269478ULL, 1000000000000000ULL, -1},
    {644334888647536ULL, 10000000000000000ULL, -1},
    {1297891447611ULL, 10000000000000000ULL, 1},
    {70769718ULL, 1000000000000000ULL, -1},
    {13515ULL, 1000000000000000ULL, 1},
};
constexpr Term kAdc2Atten3[kTermCount] = {
    {12247719764336924ULL, 10000000000000000ULL, 1},
    {755717904943462ULL, 10000000000000000ULL, -1},
    {1478791187119ULL, 10000000000000000ULL, 1},
    {79672528ULL, 1000000000000000ULL, -1},
    {15038ULL, 1000000000000000ULL, 1},
};

// floor(variable * coeff / divisor): exact in uint64 when the product fits
// (IDF's own arithmetic), in double only when it would not.
uint64_t termValue(uint64_t variable, const Term& term) {
  if (term.coeff == 0 || variable <= std::numeric_limits<uint64_t>::max() / term.coeff) {
    return variable * term.coeff / term.divisor;
  }
  return static_cast<uint64_t>(
      std::floor(static_cast<double>(variable) * static_cast<double>(term.coeff) / static_cast<double>(term.divisor)));
}

}  // namespace

uint32_t firstStepMv(uint32_t calDigi, int raw) {
  if (calDigi == 0 || raw <= 0) {
    return 0;
  }
  if (raw > kMaxRaw) {
    raw = kMaxRaw;
  }
  const uint32_t coeffA = kCoeffAScaling * kCalVoltageMv / calDigi;
  return static_cast<uint32_t>(static_cast<uint64_t>(raw) * coeffA / kCoeffAScaling);
}

int32_t rawToMv(Unit unit, uint32_t calDigi, int raw) {
  const uint64_t v = firstStepMv(calDigi, raw);
  if (v == 0) {
    return 0;
  }
  const Term* terms = unit == Unit::Adc1 ? kAdc1Atten3 : kAdc2Atten3;
  int32_t error = static_cast<int32_t>(termValue(1, terms[0])) * terms[0].sign;
  uint64_t variable = 1;
  for (int i = 1; i < kTermCount; ++i) {
    variable *= v;
    error += static_cast<int32_t>(termValue(variable, terms[i])) * terms[i].sign;
  }
  return static_cast<int32_t>(v) - error;
}

}  // namespace adc_curve
}  // namespace nhos
