#include <cassert>
#include <iostream>

#include "BoardConfig.h"

int main() {
  static_assert(NHOS_BOARD_HAS_EXT_LED == 1,
                "v1.5.F must expose the external WS2812B controller");
  static_assert(NHOS_BOARD_EXTERNAL_LED_COUNT == 9,
                "v1.5.F external WS2812B chain has nine pixels");
  assert(NHOS_BOARD_EXTERNAL_LED_COUNT == 9);
  std::cout << "v1.5.F board capability tests passed\n";
  return 0;
}
