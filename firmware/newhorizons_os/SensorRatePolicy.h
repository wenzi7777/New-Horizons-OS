#pragma once

#include <cstdint>

namespace nhos {

// How fast the IMU and the magnetometer run, derived from the scan rate.
//
// There is no setting of its own: both sensors are read once per scan (the
// poll interval is the scan interval), and each chip's output data rate is
// the lowest one that keeps up with that, so every frame carries a fresh
// sample whenever the chip can deliver one. When it cannot (BMM150 tops out
// at 30 Hz in normal mode), a frame repeats the previous sample -- accepted.
//
// Pure, no Arduino: tests_native/test_sensor_rate_policy.cpp.
struct SensorRatePolicy {
  // BMI270 accelerometer + gyroscope, and BMM350: 25/50/100/200 Hz. The scan
  // tops out at 120 fps, so 200 Hz is the most ever needed.
  static uint16_t imuOdrHzForFps(uint16_t fps) { return stepAtLeast(fps); }
  static uint16_t bmm350OdrHzForFps(uint16_t fps) { return stepAtLeast(fps); }

  // BMM350 averaging (number of samples per output) the SensorAPI allows at
  // each rate -- bmm350_set_odr_performance() quietly lowers anything above
  // these (bmm350.c), so asking for exactly these keeps status truthful.
  // Returned as the sample count: 8, 4 or 2.
  static uint8_t bmm350AveragingForOdr(uint16_t odrHz) {
    if (odrHz >= 200) return 2;
    if (odrHz >= 100) return 4;
    return 8;
  }

  // BMM150 normal mode: 10/15/20/25/30 Hz (repetitions left as they are).
  static uint16_t bmm150OdrHzForFps(uint16_t fps) {
    if (fps <= 10) return 10;
    if (fps <= 15) return 15;
    if (fps <= 20) return 20;
    if (fps <= 25) return 25;
    return 30;
  }

 private:
  static uint16_t stepAtLeast(uint16_t fps) {
    if (fps <= 25) return 25;
    if (fps <= 50) return 50;
    if (fps <= 100) return 100;
    return 200;
  }
};

}  // namespace nhos
