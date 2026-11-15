// v2.1.0 app resources: the pool every app lives in (AppPoolCore), the load
// an app is shown with (AppLoad), and how spare time is lent (AppShare).
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "AppLoad.h"
#include "AppPool.h"
#include "AppShare.h"

namespace {
int failures = 0;
void expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

struct Owner {
  uint8_t* at = nullptr;
  int moves = 0;
};

void relocate(void* owner, uint8_t* to) {
  Owner* o = static_cast<Owner*>(owner);
  o->at = to;
  ++o->moves;
}

using nhos::AppLoad;
using nhos::AppPoolCore;
using nhos::AppShare;

void poolTests() {
  alignas(4) static uint8_t memory[1024];
  AppPoolCore::Block blocks[8];
  AppPoolCore pool(memory, sizeof(memory), blocks, 8, &relocate);

  expect(sizeof(AppPoolCore::Block) <= 8 || sizeof(void*) == 8, "a block row stays small");
  expect(pool.allocateMovable(nullptr, 16) == nullptr, "a movable block needs an owner");

  // Fixed from the top, movable from the bottom.
  void* control = pool.allocateFixed(101);
  expect(control == memory + 1024 - 104, "fixed blocks come from the top, 4-aligned");
  Owner a, b, c;
  a.at = pool.allocateMovable(&a, 200);
  b.at = pool.allocateMovable(&b, 200);
  expect(a.at == memory && b.at == memory + 200, "movable blocks come from the bottom, first fit");
  expect(pool.usedBytes() == 504 && pool.freeBytes() == 520, "used and free add up");

  // A hole too small for the next one: it goes after the others.
  pool.release(a.at);
  c.at = pool.allocateMovable(&c, 300);
  expect(c.at == memory + 400, "first fit skips a hole that is too small");

  // Fragmented: 200 free at the bottom, 220 between c and the fixed block.
  // 300 fits only after compaction, which slides b and c down and says so.
  std::memset(b.at, 0xB0, 200);
  std::memset(c.at, 0xC0, 300);
  Owner d;
  d.at = pool.allocateMovable(&d, 300);
  expect(d.at != nullptr, "fits once compacted");
  expect(b.at == memory && b.moves == 1, "b relocated to the bottom");
  expect(c.at == memory + 200 && c.moves == 1, "c relocated after b");
  expect(b.at[0] == 0xB0 && b.at[199] == 0xB0, "b's bytes moved with it");
  expect(c.at[0] == 0xC0 && c.at[299] == 0xC0, "c's bytes moved with it");
  expect(d.at == memory + 500, "d after the compacted run");

  // Fixed blocks go below the others; the movables move out of the way only
  // when they must, and fixed blocks never move.
  pool.release(b.at);  // movables now c 200..500, d 500..800
  void* top = pool.allocateFixed(100);
  expect(top == memory + 820, "room above the movables: no compaction");
  expect(c.moves == 1, "nothing moved for it");
  void* low = pool.allocateFixed(150);
  expect(low == memory + 668, "below the other fixed blocks, 4-aligned");
  expect(c.at == memory && d.at == memory + 300 && c.moves == 2,
         "movables slid down to make room");
  expect(control == memory + 920 && top == memory + 820, "fixed blocks did not move");

  // A fixed hole is reused exactly.
  pool.release(top);
  void* again = pool.allocateFixed(100);
  expect(again == memory + 820, "a freed control block's hole is reused");

  // Full: 1024 - (104 + 100 + 152 + 600) = 68 left.
  expect(pool.freeBytes() == 68, "free bytes");
  expect(pool.allocateFixed(80) == nullptr, "too big: refused, not taken from the heap");
  Owner e;
  expect(pool.allocateMovable(&e, 80) == nullptr, "same for an image");
  expect(pool.allocateMovable(&e, 68) != nullptr, "exactly what is left fits");
}

void poolTableTests() {
  alignas(4) static uint8_t memory[256];
  AppPoolCore::Block blocks[2];
  AppPoolCore pool(memory, sizeof(memory), blocks, 2, &relocate);
  Owner a;
  expect(pool.allocateFixed(16) != nullptr, "first row");
  expect((a.at = pool.allocateMovable(&a, 16)) != nullptr, "second row");
  expect(pool.allocateFixed(16) == nullptr, "a full block table refuses, whatever the bytes");
}

void loadTests() {
  AppLoad load;
  // 100 ms of a 1 s window, using half of each slice it was given.
  for (int i = 0; i < 100; ++i) load.charge(1000, 2000);
  load.settle(1000000, 0);
  expect(load.cpu1 == 100 && load.cpu10 == 100 && load.cpu60 == 100,
         "the first second primes every average");
  expect(load.budget10 == 500 && load.load == 50, "half of its slices: load 50");
  expect(load.level == AppLoad::Medium, "50 is medium");
  expect(load.windowUsedUs == 0 && load.windowAllowedUs == 0, "settling clears the window");

  // Idle from now on: the 10 s average falls faster than the 60 s one.
  for (int i = 0; i < 10; ++i) load.settle(1000000, 0);
  expect(load.cpu1 == 0, "the last second is idle");
  expect(load.cpu10 < 50 && load.cpu10 > 20, "~10 s average roughly e-folds in 10 s");
  expect(load.cpu60 > load.cpu10, "~60 s average falls slower");
  expect(load.level == AppLoad::Low, "low once idle");

  // Pressure: a suspension or a skipped frame is overloaded, held 10 s.
  AppLoad busy;
  busy.charge(900, 1000);
  busy.settle(1000000, 0);
  expect(busy.load == 90 && busy.level == AppLoad::High, "90% of its slice: high");
  AppLoad pressed;
  pressed.charge(100, 1000);
  pressed.settle(1000000, 2);
  expect(pressed.level == AppLoad::Overloaded && pressed.load >= 90,
         "pressure is overloaded whatever the average");
  for (int i = 0; i < AppLoad::kPressureHoldS - 1; ++i) pressed.settle(1000000, 0);
  expect(pressed.level == AppLoad::Overloaded, "held for kPressureHoldS");
  pressed.settle(1000000, 0);
  expect(pressed.level != AppLoad::Overloaded, "and released after it");

  // Capped: a window can not read above 100%.
  AppLoad over;
  over.charge(3000000, 1000);
  over.settle(1000000, 0);
  expect(over.cpu1 == 1000 && over.budget10 == 1000 && over.load == 100, "capped at 100%");
  expect(std::strcmp(AppLoad::levelName(AppLoad::Overloaded), "overloaded") == 0, "level names");
}

void shareTests() {
  expect(AppShare::base(2000, 0) == 0 && AppShare::base(2000, 4) == 500, "base is an even split");
  expect(AppShare::cap(2000, 1) == 2000, "one app may have everything");
  expect(AppShare::cap(2000, 3) == 1000, "otherwise at most half");

  expect(AppShare::lendable(1000, 400, false) == 500, "keeps its average plus 25%");
  expect(AppShare::lendable(1000, 900, false) == 0, "nothing to spare");
  expect(AppShare::lendable(1000, 0, true) == 0, "a hungry app lends nothing");

  expect(AppShare::hungry(950, 1000, false), "using 95% of its allocation is hungry");
  expect(!AppShare::hungry(500, 1000, false), "using half is not");
  expect(AppShare::hungry(0, 1000, true), "a suspension or skipped frame is hungry");

  // Four apps, 2000 us: base 500, cap 1000. Two light ones lend 400 each.
  const uint32_t base = AppShare::base(2000, 4);
  const uint32_t cap = AppShare::cap(2000, 4);
  const uint32_t lendA = AppShare::lendable(base, 80, false);
  const uint32_t lendB = AppShare::lendable(base, 80, false);
  const uint32_t pool = lendA + lendB;
  expect(lendA == 400 && pool == 800, "two light apps lend 400 each");
  // One burst borrower: 800 offered, but the cap leaves room for 500 only.
  uint32_t loan = AppShare::loan(pool, 1, base, cap);
  expect(loan == 500, "a loan stops at the cap");
  uint32_t lentA = AppShare::lent(lendA, loan, pool);
  uint32_t lentB = AppShare::lent(lendB, loan, pool);
  expect(lentA + lentB >= loan, "lenders give at least what was borrowed");
  expect(lentA == 250 && lentB == 250, "in proportion to what each could lend");
  expect((base - lentA) + (base - lentB) + (base + loan) + base <= 2000,
         "the shares never add up to more than the total");
  // Two borrowers split the pool evenly.
  loan = AppShare::loan(pool, 2, base, cap);
  expect(loan == 400, "two borrowers split it");
  expect(AppShare::lent(lendA, loan * 2, pool) == lendA, "all of it lent when all is borrowed");
  // Nobody borrowing: nobody lends.
  expect(AppShare::loan(pool, 0, base, cap) == 0 && AppShare::lent(lendA, 0, pool) == 0,
         "no borrowers, no loans");
  // A lone app is its own cap: nothing to borrow from anyone.
  expect(AppShare::loan(0, 1, 2000, 2000) == 0, "one app: base is already everything");
  // Odd numbers still round the lenders up.
  const uint32_t lentOdd = AppShare::lent(333, 100, 999);
  expect(lentOdd == 34, "rounded up");
}

}  // namespace

int main() {
  poolTests();
  poolTableTests();
  loadTests();
  shareTests();
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_app_resources: ok\n");
  return 0;
}
