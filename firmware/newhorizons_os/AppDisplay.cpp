#include "AppDisplay.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace nhos {

namespace {
constexpr double kDigitScale[kMaxOledDigits + 1] = {1.0, 10.0, 100.0, 1000.0};
constexpr uint32_t kDigitDivisor[kMaxOledDigits + 1] = {1, 10, 100, 1000};
constexpr int16_t kGlyphPx = 6;
}  // namespace

void formatOledValue(float value, uint8_t digits, char* out, size_t outLen) {
  if (out == nullptr || outLen == 0) {
    return;
  }
  if (digits > kMaxOledDigits) {
    digits = kMaxOledDigits;
  }
  // Rounded by hand rather than by printf's "%.*f", whose behaviour on an
  // exact tie the simulator cannot promise to match. In double on purpose: a
  // float times at most 1000, plus 0.5, is EXACT in double, so the result is
  // the true value rounded once -- the same whether or not the compiler fuses
  // the multiply-add, which the S3's FPU can do and a browser cannot. Display
  // rate only (four rows at up to 5 Hz), so soft double costs nothing here.
  const bool negative = value < 0;
  const double scaled = static_cast<double>(fabsf(value)) * kDigitScale[digits];
  if (!(scaled < 1e9)) {
    snprintf(out, outLen, "#");
    return;
  }
  const uint32_t rounded = static_cast<uint32_t>(floor(scaled + 0.5));
  const uint32_t divisor = kDigitDivisor[digits];
  const char* sign = negative && rounded != 0 ? "-" : "";
  if (digits == 0) {
    snprintf(out, outLen, "%s%lu", sign, static_cast<unsigned long>(rounded));
  } else {
    snprintf(out, outLen, "%s%lu.%0*lu", sign, static_cast<unsigned long>(rounded / divisor),
             static_cast<int>(digits), static_cast<unsigned long>(rounded % divisor));
  }
}

void formatOledTextLine(const char* label, float value, uint8_t digits, char* out) {
  size_t labelLen = label != nullptr ? strlen(label) : 0;
  if (labelLen > kMaxOledLabel) {
    labelLen = kMaxOledLabel;
  }
  char number[16];
  formatOledValue(value, digits, number, sizeof(number));
  const size_t room = kOledCols - labelLen - (labelLen != 0 ? 1 : 0);
  if (strlen(number) > room) {
    snprintf(number, sizeof(number), "#");
  }
  memset(out, ' ', kOledCols);
  out[kOledCols] = '\0';
  if (labelLen != 0) {
    memcpy(out, label, labelLen);
  }
  const size_t numberLen = strlen(number);
  memcpy(out + kOledCols - numberLen, number, numberLen);
}

OledBarGeometry oledBarGeometry(uint8_t labelLen, float value, float lo, float hi) {
  if (labelLen > kMaxOledLabel) {
    labelLen = kMaxOledLabel;
  }
  OledBarGeometry geometry;
  geometry.x0 = labelLen != 0 ? static_cast<int16_t>((labelLen + 1) * kGlyphPx) : 0;
  geometry.width = static_cast<int16_t>(kOledWidthPx - geometry.x0);
  const int16_t inner = static_cast<int16_t>(geometry.width - 2);
  float fraction = (value - lo) / (hi - lo);
  // NaN fails every comparison, so it lands on empty rather than on garbage.
  if (!(fraction > 0.0f)) {
    fraction = 0.0f;
  } else if (fraction > 1.0f) {
    fraction = 1.0f;
  }
  geometry.fillPx = static_cast<int16_t>(floorf(fraction * static_cast<float>(inner)));
  return geometry;
}

}  // namespace nhos
