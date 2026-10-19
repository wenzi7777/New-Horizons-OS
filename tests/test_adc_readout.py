"""Matrix rows are converted by AdcCurveFit, not the core's analogReadMilliVolts().

The core's ESP32-S3 curve-fitting calibration overflows a uint64 near the top
of the range, so a saturated ADC2 row read ~4983 mV (v1.5.F GPIO13/14,
recording 120922). The arithmetic is pinned by tests_native/test_adc_curve_fit.cpp;
this pins that every read path goes through it.
"""
import re
import unittest
from pathlib import Path

ARDUINO_ROOT = Path(__file__).resolve().parents[1] / "firmware" / "newhorizons_os"


class AdcReadoutTests(unittest.TestCase):
    def setUp(self):
        self.impl = (ARDUINO_ROOT / "MatrixScanner.cpp").read_text(encoding="utf-8")

    def body(self, signature):
        start = self.impl.index(signature)
        return self.impl[start:self.impl.index("\n}\n", start)]

    def test_every_read_path_uses_read_cell_mv(self):
        for signature in ("size_t MatrixScanner::scanIntoPacketPayload(",
                          "bool MatrixScanner::sampleRawFrame(",
                          "bool MatrixScanner::sampleRawCell("):
            with self.subTest(signature=signature):
                body = self.body(signature)
                self.assertIn("readCellMv(", body)
                self.assertNotIn("analogRead", body)

    def test_core_millivolts_only_without_an_efuse_point(self):
        # analogReadMilliVolts() survives only as the no-calibration fallback.
        self.assertEqual(len(re.findall(r"\banalogReadMilliVolts\(\w", self.impl)), 1)
        read = self.body("float MatrixScanner::readCellMv(")
        self.assertLess(read.index("if (digi == 0)"), read.index("analogReadMilliVolts("))
        self.assertIn("adc_curve::rawToMv(", read)

    def test_failed_read_holds_the_last_value_and_counts(self):
        read = self.body("float MatrixScanner::readCellMv(")
        self.assertIn("if (raw > adc_curve::kMaxRaw)", read)
        self.assertIn("++adcReadErrors_[unit];", read)
        self.assertIn("lastCellMv_[cellIndex]", read)

    def test_scan_health_reports_errors_and_calibration_point(self):
        health = self.body("String MatrixScanner::healthJson()")
        self.assertIn('\\"adc_read_errors\\":[', health)
        self.assertIn('\\"adc_cal_digi\\":[', health)

    def test_native_test_is_wired(self):
        run = (ARDUINO_ROOT.parents[1] / "tests_native" / "run.sh").read_text(encoding="utf-8")
        self.assertIn('"$FIRMWARE_DIR/AdcCurveFit.cpp"', run)
        self.assertIn('"$BUILD_DIR/test_adc_curve_fit"', run)


if __name__ == "__main__":
    unittest.main()
