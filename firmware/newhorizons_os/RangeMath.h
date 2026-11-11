#pragma once

#include <cstddef>
#include <cstdint>

namespace nhos {

// v2.0.F front end, per row: the op-amp's + input sits at Vref, a pressed
// cell (Rs) to a driven-low column draws Vref/Rs through Rfb, so
//
//     Vout = Vref * (1 + Rfb / Rs)                     (clips near the rail)
//
// Rfb is not fitted at the factory -- the user solders it -- so everything
// here starts from a measured Rfb per row. Pure functions, no Arduino.

// The calibration network: 14 x 10k 0.1 % from each GIN to CAL, CAL to GND
// through Q1. With it on and every column released, Rs = 10k.
static constexpr float kCalResistorOhm = 10000.0f;
// GPIO45's boot pull-down (R27) is on column 13 whenever that column is idle.
static constexpr float kLeakResistorOhm = 100000.0f;
static constexpr uint8_t kLeakColumnPin = 45;
// The ADC stops being trustworthy about here (a saturated row reads ~3.1 V).
static constexpr float kSaturationMv = 3000.0f;

// Rfb from one row's reading with the calibration network on:
// Vout = Vref (1 + Rfb / 10k)  ->  Rfb = 10k (Vout / Vref - 1).
// Returns 0 when it cannot be trusted: no Rfb fitted (Vout stays at Vref),
// Rfb too large for this Vref (Vout saturates), or bad inputs.
float rfbFromCalibration(float calOnMv, float vrefMv);

// Re-expresses a reading taken at (vrefMv, rfbOhm) as the reading the same
// cell would give at (refVrefMv, refRfbOhm): the part above Vref scales by
// (refVref * refRfb) / (vref * rfb). Readings at or below Vref are no signal
// and map to refVrefMv. Returns the input unchanged if rfbOhm is unknown.
//
// offsetMv is the row's ADC offset: with nothing pressed the row reads
// vref + offsetMv (the ESP32 ADC is a few to ten mV off at this end of the range,
// and the sign differs between ADC1 and ADC2), so that is where "no signal" is.
float virtualizeMv(float voutMv, float vrefMv, float rfbOhm, float refVrefMv, float refRfbOhm,
                   float offsetMv = 0.0f);

// How much the idle column 13 adds to a row's Vout while another column is
// scanned: its sensor Rs13 in series with the 100k pull-down.
//   Rs13 = Rfb * Vref / (V13 - Vref)
//   leak = Vref * Rfb / (Rs13 + 100k)
// v13Mv is that row's reading while column 13 itself was driven low.
float leakMv(float v13Mv, float vrefMv, float rfbOhm, float leakOhm = kLeakResistorOhm,
             float offsetMv = 0.0f);

// Rfb from several Vref points, each read with the calibration network off and on.
// The readings are  off = g*V + e  and  on = g*V*(1 + Rfb/10k) + e,  so the
// slopes of both against V cancel the ADC offset e and gain g (and the DAC's):
//   Rfb = 10k * (slopeOn / slopeOff - 1).
// A least-squares fit over the points averages out the ADC's few-mV scatter, which
// two points alone cannot. `offsetMv` returns e as the row's reading above Vref
// with nothing pressed. Returns 0 when there are fewer than two points, they span
// less than 100 mV, or any reading is at the rail.
float rfbFromPoints(const float* vrefMv, const float* offMv, const float* onMv, size_t count,
                    float* offsetMv);

inline bool isSaturatedMv(float voutMv) { return voutMv >= kSaturationMv; }

// Auto range: one step of the Vref chooser. It only ever lowers Vref below the
// safe value (more headroom) and returns to it; it never goes above it.
//   saturatedRows: rows that clipped in the last window
//   quietMs:       how long no row has clipped
// Returns the Vref to use next.
float autoRangeNextVrefMv(float currentVrefMv, size_t saturatedRows, uint32_t quietMs,
                          float safeVrefMv, float minVrefMv);

// Auto range acts on the last frame's saturation mask only while frames keep
// coming: once the scan has stopped (or stalled) for this long the mask is
// stale and Vref is left where it is. 1.5 s: the slowest scan (1 fps) still
// counts as running.
static constexpr uint32_t kAutoRangeFrameStaleMs = 1500;
// frameAgeMs: time since the last endFrame; haveFrame false = none yet.
inline bool autoRangeFrameFresh(bool haveFrame, uint32_t frameAgeMs) {
  return haveFrame && frameAgeMs <= kAutoRangeFrameStaleMs;
}

// Mean of the ADC reads that succeeded (ADC2 loses reads to Wi-Fi; a failed
// read is skipped, not counted as 0 mV). mean() is false when none succeeded.
struct ReadingMean {
  float total = 0.0f;
  uint32_t good = 0;
  uint32_t failed = 0;
  void add(bool ok, float mv) {
    if (ok) {
      total += mv;
      ++good;
    } else {
      ++failed;
    }
  }
  bool mean(float& out) const {
    if (good == 0) {
      return false;
    }
    out = total / static_cast<float>(good);
    return true;
  }
};

// rfb_measure: the highest Vref of the fit. Where the largest Rfb stays off the
// rail (about 2.6 V out), but never above the operator's limit nor at or above
// strapCeilingMv (GPIO45 strap: Vref must stay below about 0.8 V).
float rfbFitTopMv(float biggestRfbOhm, float limitMv, float strapCeilingMv);

}  // namespace nhos
