#pragma once

#include <Arduino.h>

namespace nhos {

// Low-water mark of the largest contiguous free heap block since boot.
//
// ESP.getMinFreeHeap() is the low-water mark of the TOTAL free heap, which
// says nothing about fragmentation: 50 KB free in a dozen pieces cannot hold
// one 16 KB buffer. The largest block is only known when asked, so this is
// sampled (once a second, from the lifetime task) and kept for memory_status.
// Runtime only -- a reboot starts it again.
class HeapWatermark {
 public:
  static void sample() {
    const uint32_t largest = ESP.getMaxAllocHeap();
    if (minLargest_ == 0 || largest < minLargest_) {
      minLargest_ = largest;
    }
  }
  // 0 until the first sample.
  static uint32_t minLargestFreeBlock() { return minLargest_; }

 private:
  static inline uint32_t minLargest_ = 0;
};

}  // namespace nhos
