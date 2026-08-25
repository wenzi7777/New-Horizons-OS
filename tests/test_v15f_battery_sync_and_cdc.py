import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_ROOT = REPO_ROOT / "firmware" / "newhorizons_os"


class V15fBatterySyncAndCdcTests(unittest.TestCase):
    def test_battery_gauge_wires_quick_start_and_sync_status(self):
        header = (FIRMWARE_ROOT / "BatteryGaugeManager.h").read_text(encoding="utf-8")
        impl = (FIRMWARE_ROOT / "BatteryGaugeManager.cpp").read_text(encoding="utf-8")

        self.assertIn("GaugeResyncResult requestResync", header)
        self.assertIn("bool writeWord", header)
        self.assertIn("kMax17048ModeRegister = 0x06", impl)
        self.assertIn("kMax17048QuickStart = 0x4000", impl)
        self.assertIn("syncPolicy_.observeVoltage", impl)
        self.assertIn('out += ",\\\"gauge_sync_state\\\":\\\""', impl)
        self.assertIn('\\\"last_sync_reason\\\":\\\"', impl)
        self.assertIn('\\\"gauge_sync_count\\\":', impl)
        self.assertIn('out += sample_.valid ? "true" : "null"', impl)
        self.assertIn('out += sample_.valid ? String(sample_.socCentiPercent) : "null"', impl)
        self.assertIn("syncPolicy_.state() != GaugeSyncState::Ready", impl)
        self.assertIn('diagnostic_ = "max17048_resync_error"', impl)

    def test_control_server_exposes_idempotent_resync_command(self):
        control = (FIRMWARE_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")

        command_start = control.index('if (cmd == "resync_battery_gauge")')
        command_end = control.index('if (cmd == "detect_battery_profile")', command_start)
        command = control[command_start:command_end]
        self.assertIn("batteryProfileCommandSupported(NHOS_BOARD_HAS_MAX17048)", command)
        self.assertIn("battery_gauge_resync_started", command)
        self.assertIn("battery_gauge_resync_in_progress", command)
        self.assertIn("battery_gauge_resync_failed", command)

    def test_v15f_runtime_never_waits_for_hwcdc_reader(self):
        sketch = (FIRMWARE_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")
        scanner = (FIRMWARE_ROOT / "MatrixScanner.cpp").read_text(encoding="utf-8")

        self.assertIn("Serial.setTxTimeoutMs(0)", sketch)
        self.assertNotIn("struct LoopProfile", sketch)
        self.assertNotIn("reportLoopProfileIfDue", sketch)
        self.assertIn("#if NHOS_ENABLE_SERIAL_PERF_DIAGNOSTICS", scanner)


if __name__ == "__main__":
    unittest.main()
