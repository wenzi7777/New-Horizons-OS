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

  // WS2812B-2020-V6 is GRB, MSB first. Its V6 timing specification needs
  // a 0 high period of 220..380 ns and a 1 low period of 580 ns or more.
  // At the 10 MHz RMT clock this is 300/1000 ns for 0 and 700/600 ns for 1;
  // both 1.3 us cycles comply with the documented >= 1.25 us period.
  assert(symbols[0] == (Ws2812RmtSymbol{1, 7, 0, 6}));
  assert(symbols[1] == (Ws2812RmtSymbol{1, 3, 0, 10}));
  assert(symbols[7] == (Ws2812RmtSymbol{1, 3, 0, 10}));
  assert(symbols[23] == (Ws2812RmtSymbol{1, 7, 0, 6}));

  // An incomplete destination must be rejected rather than producing a
  // truncated frame that the first LED could relay as corrupted colours.
  assert(encodeWs2812Grb(grb, sizeof(grb), symbols, 23) == 0);
  assert(encodeWs2812Grb(nullptr, sizeof(grb), symbols, 24) == 0);
  return 0;
}
