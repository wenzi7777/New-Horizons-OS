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
float virtualizeMv(float voutMv, float vrefMv, float rfbOhm, float refVrefMv, float refRfbOhm);

// How much the idle column 13 adds to a row's Vout while another column is
// scanned: its sensor Rs13 in series with the 100k pull-down.
//   Rs13 = Rfb * Vref / (V13 - Vref)
//   leak = Vref * Rfb / (Rs13 + 100k)
// v13Mv is that row's reading while column 13 itself was driven low.
float leakMv(float v13Mv, float vrefMv, float rfbOhm, float leakOhm = kLeakResistorOhm);

inline bool isSaturatedMv(float voutMv) { return voutMv >= kSaturationMv; }

// Auto range: one step of the Vref chooser. It only ever lowers Vref below the
// safe value (more headroom) and returns to it; it never goes above it.
//   saturatedRows: rows that clipped in the last window
//   quietMs:       how long no row has clipped
// Returns the Vref to use next.
float autoRangeNextVrefMv(float currentVrefMv, size_t saturatedRows, uint32_t quietMs,
                          float safeVrefMv, float minVrefMv);

}  // namespace nhos
