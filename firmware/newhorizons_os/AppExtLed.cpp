#include "AppExtLed.h"

#include <math.h>

namespace nhos {

namespace {
constexpr uint8_t kMeterLow[3] = {0, 255, 0};
constexpr uint8_t kMeterHigh[3] = {255, 0, 0};
}  // namespace

uint8_t appExtMeterLit(float value, float lo, float hi, uint8_t count) {
  const float span = hi - lo;
  if (!(span > 0.0f)) {
    return 0;
  }
  const float fraction = (value - lo) / span;
  if (!(fraction > 0.0f)) {
    return 0;
  }
  if (fraction >= 1.0f) {
    return count;
  }
  const float lit = ceilf(fraction * static_cast<float>(count));
  return lit >= static_cast<float>(count) ? count : static_cast<uint8_t>(lit);
}

void appExtMeterColour(uint8_t index, uint8_t count, uint8_t rgb[3]) {
  const float t = count > 1 ? static_cast<float>(index) / static_cast<float>(count - 1) : 0.0f;
  for (uint8_t c = 0; c < 3; ++c) {
    const float channel =
        static_cast<float>(kMeterLow[c]) +
        static_cast<float>(static_cast<int>(kMeterHigh[c]) - static_cast<int>(kMeterLow[c])) * t;
    rgb[c] = static_cast<uint8_t>(channel);
  }
}

void renderAppExtLeds(const AppExtLedFrame& frame, uint8_t count, uint8_t out[][3]) {
  for (uint8_t i = 0; i < count; ++i) {
    out[i][0] = out[i][1] = out[i][2] = 0;
  }
  if (frame.hasMeter) {
    const uint8_t lit = appExtMeterLit(frame.meterValue, frame.meterLo, frame.meterHi, count);
    for (uint8_t i = 0; i < lit; ++i) {
      appExtMeterColour(i, count, out[i]);
    }
  }
  for (uint8_t i = 0; i < count && i < kMaxAppExtLeds; ++i) {
    if ((frame.pixelMask & (1U << i)) == 0) {
      continue;
    }
    out[i][0] = frame.pixelRgb[i][0];
    out[i][1] = frame.pixelRgb[i][1];
    out[i][2] = frame.pixelRgb[i][2];
  }
}

}  // namespace nhos
