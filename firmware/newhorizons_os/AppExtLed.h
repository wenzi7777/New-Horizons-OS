#pragma once

#include <stddef.h>
#include <stdint.h>

namespace nhos {

// What the apps put on the external LED strip: a meter along it and single
// pixels over it.
//
// As with the OLED, an app states WHAT to show and the firmware owns the
// drawing: pixels and meters are scalar nodes that record their state, and
// the strip is rendered from it at the LED service's own pace, outside every
// app's budget.
//
// The rendering below is a compatibility contract with the App Library's
// sdk/lib/extled.mjs, which the Desktop uses to preview the strip offline.
// Both sides do the same float32 arithmetic in the same order, and
// sdk/test/firmware-contract.test.mjs pins the constants and the output.

// The most pixels any board has (v1.5.F). A pixel past a board's own count is
// accepted and never shown, so one package runs on every board.
static constexpr uint8_t kMaxAppExtLeds = 9;

struct AppExtLedFrame {
  // True while a running app that may drive the strip holds it -- even on a
  // frame where it lit nothing, which shows as dark rather than handing the
  // strip back to the configured preset for a frame.
  bool active = false;
  bool hasMeter = false;
  float meterValue = 0;
  float meterLo = 0;
  float meterHi = 1;
  // Pixels an app set this frame, and their colours.
  uint16_t pixelMask = 0;
  uint8_t pixelRgb[kMaxAppExtLeds][3] = {};
};

// How many of `count` pixels a meter lights for `value` over [lo, hi]: none
// at or below lo (or for NaN), all at or above hi, and otherwise the fraction
// rounded up, so any value above lo lights at least one.
uint8_t appExtMeterLit(float value, float lo, float hi, uint8_t count);

// The colour of pixel `index` of a `count`-pixel meter: green at the start to
// red at the end.
void appExtMeterColour(uint8_t index, uint8_t count, uint8_t rgb[3]);

// The strip as `frame` draws it on a `count`-pixel board: the meter, then the
// pixels over it. `out` must hold `count` entries; everything unlit is 0.
void renderAppExtLeds(const AppExtLedFrame& frame, uint8_t count, uint8_t out[][3]);

}  // namespace nhos
