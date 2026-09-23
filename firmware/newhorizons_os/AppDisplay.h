#pragma once

#include <stddef.h>
#include <stdint.h>

namespace nhos {

// What an app may put on the OLED: one row of the 128x32 panel at text size 1,
// either a label with a number or a label with a bar.
//
// Deliberately rows, not pixels. An app states WHAT to show; the firmware owns
// the drawing, so a package cannot cost an I2C transfer per frame (the panel is
// redrawn at the OLED's own update_hz, outside every app's budget) and the
// App Library's simulator can reproduce the screen exactly.
//
// The formatting below is a compatibility contract with the App Library's
// sdk/lib/oled.mjs, which the Desktop uses to preview a screen offline. Both
// sides do the same float32 arithmetic in the same order, and
// sdk/test/firmware-contract.test.mjs pins the constants.
enum class AppDisplayKind : uint8_t {
  None = 0,
  Text,
  Bar,
};

struct AppDisplayLine {
  AppDisplayKind kind = AppDisplayKind::None;
  const char* label = "";
  float value = 0;
  uint8_t digits = 0;
  float lo = 0;
  float hi = 0;
};

static constexpr uint8_t kOledRows = 4;
static constexpr uint8_t kOledCols = 21;       // 128px / 6px per glyph
static constexpr uint8_t kOledRowPx = 8;
static constexpr int16_t kOledWidthPx = 128;
static constexpr uint8_t kMaxOledLabel = 10;
static constexpr uint8_t kMaxOledDigits = 3;

// `value` with `digits` decimals, rounded half away from zero. A value whose
// magnitude times 10^digits reaches 1e9 -- and NaN or infinity -- is "#".
// `out` must hold at least 16 bytes.
void formatOledValue(float value, uint8_t digits, char* out, size_t outLen);

// A Text row as it appears on the panel: the label at the left, the value
// right-aligned to the last column, padded to exactly kOledCols characters. A
// value that does not fit beside the label is shown as "#". `out` must hold
// kOledCols + 1 bytes.
void formatOledTextLine(const char* label, float value, uint8_t digits, char* out);

// A Bar row's geometry: where the outline starts, and how many of its inner
// pixels are filled for `value` in [lo, hi].
struct OledBarGeometry {
  int16_t x0 = 0;      // left edge of the outline
  int16_t width = 0;   // outline width, reaching the right edge of the panel
  int16_t fillPx = 0;  // filled pixels inside the outline, 0..width-2
};
OledBarGeometry oledBarGeometry(uint8_t labelLen, float value, float lo, float hi);

}  // namespace nhos
