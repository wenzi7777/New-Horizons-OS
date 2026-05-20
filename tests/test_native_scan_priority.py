import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCAN_SOURCE = REPO_ROOT / "firmware" / "native" / "vdboard" / "scan.c"


class NativeScanPriorityTests(unittest.TestCase):
    def test_scan_runs_from_main_loop_service_without_task_stack(self):
        source = SCAN_SOURCE.read_text(encoding="utf-8")

        self.assertNotIn("VDBOARD_SCAN_TASK_PRIORITY", source)
        self.assertNotIn("xTaskCreatePinnedToCore", source)
        self.assertIn("static mp_obj_t vdboard_scan_service(void)", source)
        self.assertIn("vdboard_capture_payload(g_scan_ctx.payload_mv);", source)
        self.assertIn("scan_interval_us", source)


if __name__ == "__main__":
    unittest.main()
