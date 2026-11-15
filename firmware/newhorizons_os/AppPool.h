#pragma once

#include <stddef.h>
#include <stdint.h>

namespace nhos {

// The one memory pool every app lives in (v2.1.0). It replaced the four
// static AppVm slots and their separate 4 KB image pool: how many apps run is
// now whatever fits here, not a number fixed at build time.
//
// Two kinds of block share it, from opposite ends:
//  * fixed blocks, taken from the top and never moved: an app's control
//    block (the AppVm object itself, kAppControlBytes) and, only for an app
//    with the `storage` capability, its store (kAppStoreBytes). Other code
//    holds pointers to these, so they must stay put.
//  * movable blocks, taken from the bottom: an app's image, arena and stack.
//    When the free space is fragmented they are slid down together and their
//    owner told where (RelocateFn); the interpreter keeps only offsets into
//    them, so moving one between deliveries is safe.
// Every movable block lies below every fixed block, so the free space between
// the two ends is one run once the movables are compacted.
//
// Allocation happens only when an app is activated or replaced (a command
// handler on the loop task, never inside a delivery), and nothing here calls
// malloc. Pure C++ on a caller's buffer, so the host tests run it as is.
class AppPoolCore {
 public:
  using RelocateFn = void (*)(void* owner, uint8_t* to);

  // 8 bytes: the table has a row per block, and a pool full of small apps
  // has many. A pool is at most 64 KB; fixed blocks are the ones with no owner.
  struct Block {
    uint16_t at = 0;
    uint16_t len = 0;
    void* owner = nullptr;  // movable blocks only
    bool fixed() const { return owner == nullptr; }
  };

  AppPoolCore(uint8_t* memory, uint32_t bytes, Block* blocks, uint16_t maxBlocks,
              RelocateFn relocate)
      : memory_(memory), bytes_(bytes), blocks_(blocks), maxBlocks_(maxBlocks),
        relocate_(relocate) {}

  // nullptr when it does not fit (or the block table is full). A movable
  // block needs an owner: relocation tells it where its bytes went.
  void* allocateFixed(uint32_t len);
  uint8_t* allocateMovable(void* owner, uint32_t len);
  // Either kind; unknown pointers are ignored.
  void release(const void* memory);
  // Slides every movable block down to the start of the pool.
  void compact();

  uint32_t bytes() const { return bytes_; }
  uint32_t usedBytes() const;
  uint32_t freeBytes() const { return bytes_ - usedBytes(); }
  uint16_t blockCount() const { return count_; }
  bool owns(const void* memory) const;

  static uint32_t align4(uint32_t n) { return (n + 3U) & ~3U; }

 private:
  uint32_t boundary() const;     // lowest fixed block, or the end of the pool
  uint32_t movableEnd() const;   // end of the highest movable block, or 0
  uint32_t movableUsed() const;
  bool insert(const Block& block);

  uint8_t* memory_;
  uint32_t bytes_;
  Block* blocks_;  // sorted by `at`
  uint16_t maxBlocks_;
  uint16_t count_ = 0;
  RelocateFn relocate_;
};

}  // namespace nhos
