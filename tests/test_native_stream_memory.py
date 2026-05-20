import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
NATIVE_SCAN_PATH = REPO_ROOT / "firmware" / "native" / "vdboard" / "scan.c"


class NativeStreamMemoryTests(unittest.TestCase):
    def test_calibration_table_is_allocated_compactly(self):
        source = NATIVE_SCAN_PATH.read_text()

        self.assertIn("calibration_offsets", source)
        self.assertIn("calibration_point_capacity", source)
        self.assertNotIn("point_count * VDBOARD_STREAM_MAX_CAL_POINTS", source)
        self.assertIn("calibration_point_capacity * sizeof(vdboard_stream_cal_point_t)", source)


if __name__ == "__main__":
    unittest.main()
