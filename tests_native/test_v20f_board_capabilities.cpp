#include <cassert>
#include <cstring>
#include <iostream>

#include "BoardConfig.h"

// Built with -DNHOS_BOARD_V20F.
int main() {
  static_assert(NHOS_BOARD_ROWS == 14 && NHOS_BOARD_COLS == 14,
                "v2.0.F keeps the 14x14 matrix");
  static_assert(NHOS_BOARD_HAS_EXT_LED == 1 && NHOS_BOARD_EXTERNAL_LED_COUNT == 9,
                "v2.0.F exposes the nine-pixel external chain");
  static_assert(NHOS_BOARD_STATUS_LED_COUNT == 2 && NHOS_BOARD_HAS_BATTERY_LED == 1,
                "v2.0.F keeps the dedicated battery pixel");
  static_assert(NHOS_BOARD_HAS_CONFIGURABLE_BUTTON == 1 && NHOS_BOARD_EXT_LED_RMT == 1,
                "the v1.5.F button and RMT strip carry over");
  static_assert(NHOS_BOARD_HAS_DAC_VREF == 1 && NHOS_BOARD_HAS_PCA9536 == 1 &&
                    NHOS_BOARD_HAS_BATID == 1,
                "v2.0.F adds the Vref DAC, the calibration switch and BAT_ID");
  static_assert(NHOS_BOARD_HAS_MAX17048 == 1 && NHOS_BOARD_HAS_BMM350 == 1,
                "gauge and magnetometer are unchanged");
  assert(std::strcmp(NHOS_BOARD_NAME, "TIA-CTL v2.0.F 2026.10") == 0);
  // Older firmware only accepts a manifest whose model is its own pre-rename name.
  assert(std::strcmp(NHOS_BOARD_OTA_MODEL, "VD-CTL/R v2.0.F 2026.10") == 0);
  assert(std::strstr(NHOS_BOARD_OTA_MANIFEST_BASE, "arduino-v20f") != nullptr);
  std::cout << "v2.0.F board capability tests passed\n";
  return 0;
}
