#pragma once

#include <cstddef>
#include <cstdint>

namespace nhos {

// A platform-neutral RMT symbol used to encode a WS2812B frame. Keeping the
// encoder independent of Arduino lets the exact frame shape be regression
// tested on the host before it is copied to ESP32's rmt_data_t representation.
struct Ws2812RmtSymbol {
  uint8_t level0;
  uint16_t duration0;
  uint8_t level1;
  uint16_t duration1;

  constexpr bool operator==(const Ws2812RmtSymbol& other) const {
    return level0 == other.level0 && duration0 == other.duration0 &&
           level1 == other.level1 && duration1 == other.duration1;
  }
};

constexpr uint16_t kWs2812RmtOneHighTicks = 8;   // 800 ns at 10 MHz
constexpr uint16_t kWs2812RmtOneLowTicks = 4;    // 400 ns at 10 MHz
constexpr uint16_t kWs2812RmtZeroHighTicks = 4;  // 400 ns at 10 MHz
constexpr uint16_t kWs2812RmtZeroLowTicks = 8;   // 800 ns at 10 MHz

// Encodes a GRB byte stream MSB first. Returns zero when the input/output is
// incomplete, so callers never send a truncated WS2812 frame.
inline size_t encodeWs2812Grb(const uint8_t* grb, size_t byteCount,
                              Ws2812RmtSymbol* output,
                              size_t outputCapacity) {
  if (!grb || !output || byteCount == 0 || outputCapacity < byteCount * 8U) {
    return 0;
  }

  size_t written = 0;
  for (size_t byteIndex = 0; byteIndex < byteCount; ++byteIndex) {
    const uint8_t value = grb[byteIndex];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const bool one = (value & static_cast<uint8_t>(0x80U >> bit)) != 0;
      output[written++] = one
                              ? Ws2812RmtSymbol{1, kWs2812RmtOneHighTicks, 0,
                                                 kWs2812RmtOneLowTicks}
                              : Ws2812RmtSymbol{1, kWs2812RmtZeroHighTicks, 0,
                                                 kWs2812RmtZeroLowTicks};
    }
  }
  return written;
}

}  // namespace nhos
