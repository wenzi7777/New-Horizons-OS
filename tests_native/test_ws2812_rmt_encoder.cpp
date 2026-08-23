#include <cassert>
#include <cstddef>
#include <cstdint>

#include "Ws2812RmtEncoder.h"

int main() {
  using nhos::Ws2812RmtSymbol;
  using nhos::encodeWs2812Grb;

  const uint8_t grb[] = {0x80, 0x00, 0x01};
  Ws2812RmtSymbol symbols[24] = {};

  const size_t written = encodeWs2812Grb(grb, sizeof(grb), symbols, 24);
  assert(written == 24);

  // WS2812B-2020-V6 is GRB, MSB first. At the 10 MHz RMT clock used by
  // the controller a logical 1 is 800 ns high / 400 ns low, and 0 is
  // 400 ns high / 800 ns low.
  assert(symbols[0] == (Ws2812RmtSymbol{1, 8, 0, 4}));
  assert(symbols[1] == (Ws2812RmtSymbol{1, 4, 0, 8}));
  assert(symbols[7] == (Ws2812RmtSymbol{1, 4, 0, 8}));
  assert(symbols[23] == (Ws2812RmtSymbol{1, 8, 0, 4}));

  // An incomplete destination must be rejected rather than producing a
  // truncated frame that the first LED could relay as corrupted colours.
  assert(encodeWs2812Grb(grb, sizeof(grb), symbols, 23) == 0);
  assert(encodeWs2812Grb(nullptr, sizeof(grb), symbols, 24) == 0);
  return 0;
}
