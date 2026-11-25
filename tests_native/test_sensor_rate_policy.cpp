// SensorRatePolicy: chip output data rates derived from the scan rate.
#include <cstdint>
#include <cstdio>

#include "SensorRatePolicy.h"

namespace {
int failures = 0;
void expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}
using P = nhos::SensorRatePolicy;
}  // namespace

int main() {
  // BMI270 / BMM350: the lowest of 25/50/100/200 Hz at or above the fps.
  expect(P::imuOdrHzForFps(1) == 25, "1 fps -> 25 Hz");
  expect(P::imuOdrHzForFps(25) == 25, "25 fps -> 25 Hz");
  expect(P::imuOdrHzForFps(26) == 50, "26 fps -> 50 Hz");
  expect(P::imuOdrHzForFps(50) == 50, "50 fps -> 50 Hz");
  expect(P::imuOdrHzForFps(60) == 100, "60 fps (default) -> 100 Hz, as before");
  expect(P::imuOdrHzForFps(100) == 100, "100 fps -> 100 Hz");
  expect(P::imuOdrHzForFps(101) == 200, "101 fps -> 200 Hz");
  expect(P::imuOdrHzForFps(120) == 200, "120 fps (max) -> 200 Hz");
  for (uint16_t fps = 1; fps <= 120; ++fps) {
    expect(P::imuOdrHzForFps(fps) >= fps, "IMU keeps up with every scan rate");
    expect(P::bmm350OdrHzForFps(fps) == P::imuOdrHzForFps(fps), "BMM350 uses the same steps");
  }

  // BMM350 averaging: the most the SensorAPI allows at that rate.
  expect(P::bmm350AveragingForOdr(25) == 8, "25 Hz -> 8");
  expect(P::bmm350AveragingForOdr(50) == 8, "50 Hz -> 8");
  expect(P::bmm350AveragingForOdr(100) == 4, "100 Hz -> 4");
  expect(P::bmm350AveragingForOdr(200) == 2, "200 Hz -> 2");

  // BMM150: 10/15/20/25/30 Hz, capped at 30 (repeats above that).
  expect(P::bmm150OdrHzForFps(1) == 10, "BMM150 1 fps -> 10 Hz");
  expect(P::bmm150OdrHzForFps(10) == 10, "BMM150 10 fps -> 10 Hz");
  expect(P::bmm150OdrHzForFps(11) == 15, "BMM150 11 fps -> 15 Hz");
  expect(P::bmm150OdrHzForFps(25) == 25, "BMM150 25 fps -> 25 Hz");
  expect(P::bmm150OdrHzForFps(30) == 30, "BMM150 30 fps -> 30 Hz");
  expect(P::bmm150OdrHzForFps(31) == 30, "BMM150 31 fps -> capped at 30 Hz");
  expect(P::bmm150OdrHzForFps(120) == 30, "BMM150 120 fps -> capped at 30 Hz");

  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_sensor_rate_policy: ok\n");
  return 0;
}
