#pragma once

#include <cstddef>
#include <cstdint>

#include "PacketWireConstants.h"

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

// Conductance of a row's pressed cell, in microsiemens:
//   Vout - (Vref + offset) = Vref * Rfb / Rs   ->   G = 1/Rs = signal / (Vref * Rfb)
// Vref and Rfb both cancel out of what the cell is, so the answer does not move
// when auto range changes Vref. offsetMv is the row's ADC offset (with nothing
// pressed the row reads vref + offsetMv). Readings at or below that are no
// signal (0); a row with no measured Rfb has no conductance either (0).
float conductanceUs(float voutMv, float vrefMv, float rfbOhm, float offsetMv = 0.0f);

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

// Auto range: one step of the Vref chooser (v2.0.F, conductance output).
// It moves both ways:
//   - a measured row clipped in the last frame: Vref x 0.8, down to the floor;
//   - nothing clipped for kAutoRangeRaiseHoldMs, and the window has run as long
//     at this Vref: raise it toward where the window's peak would sit at
//     kAutoRangeTargetPeakMv (Vout scales with Vref), at most x1.25 a step and
//     never above the ceiling. Peaks between the target and the rail are left
//     alone (hysteresis), so it does not hunt at the edge of saturation.
//   - above the ceiling (the operator lowered the limit): back to the ceiling.
// Only rows with a measured Rfb count, for the peak and the saturation alike:
// a row without Rfb has no feedback and sits at the rail.
static constexpr float kAutoRangeLowerFactor = 0.8f;
static constexpr float kAutoRangeMaxRaiseFactor = 1.25f;
static constexpr float kAutoRangeMinRaiseFactor = 1.03f;
static constexpr float kAutoRangeTargetPeakMv = 2000.0f;
static constexpr uint32_t kAutoRangeRaiseHoldMs = 5000;
// Auto range's own ceiling, whatever limit the operator set: kept below the
// GPIO45 strap risk (about 0.8 V), the same top rfb_measure fits up to.
static constexpr float kAutoRangeMaxVrefMv = 750.0f;

struct AutoRangeInput {
  float currentVrefMv = 0.0f;
  size_t saturatedRows = 0;  // measured rows that clipped in the last frame
  uint32_t quietMs = 0;      // since a measured row last clipped
  float windowPeakMv = 0.0f; // highest measured-row reading since Vref last moved
  uint32_t windowMs = 0;     // how long that window has run
  float floorMv = 0.0f;
  float ceilingMv = 0.0f;
};
float autoRangeNextVrefMv(const AutoRangeInput& in);

// min(operator limit, kAutoRangeMaxVrefMv), and never below the floor.
float autoRangeCeilingMv(float limitMv, float floorMv);

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

// The range TLV (wire.json packet.range, PROTOCOL.md 3.1): type, length, then
// unit u8, flags u8, vref_mv u16, saturated_rows u32, little-endian. Returns
// the bytes written (2 + kRangeTlvLen), or 0 when it does not fit.
size_t encodeRangeTlv(bool conductance, bool autoRange, uint16_t vrefMv, uint32_t saturatedRows,
                      uint8_t* out, size_t capacity);

// rfb_measure: the highest Vref of the fit. Where the largest Rfb stays off the
// rail (about 2.6 V out), but never above the operator's limit nor at or above
// strapCeilingMv (GPIO45 strap: Vref must stay below about 0.8 V).
float rfbFitTopMv(float biggestRfbOhm, float limitMv, float strapCeilingMv);

}  // namespace nhos
