import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCAN_SOURCE = REPO_ROOT / "firmware" / "native" / "vdboard" / "scan.c"


class NativeScanPriorityTests(unittest.TestCase):
    def test_scan_task_priority_stays_below_system_critical_levels(self):
        source = SCAN_SOURCE.read_text(encoding="utf-8")

        self.assertIn("#define VDBOARD_SCAN_TASK_PRIORITY (tskIDLE_PRIORITY + 2)", source)


if __name__ == "__main__":
    unittest.main()
