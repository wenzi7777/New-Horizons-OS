import unittest
from pathlib import Path

import sys as _sys
_sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent))
from _control_source import command_names, control_source, flagged, handler_body  # noqa: E402
import sys as _fsys
_fsys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent))
from _firmware import CORE_ROOT, firmware_file, firmware_sources, read_firmware  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_ROOT = REPO_ROOT / "firmware" / "newhorizons_os"


def read(name: str) -> str:
    return read_firmware(name)


class ManualBatteryProfileTests(unittest.TestCase):
    """Up to v1.5.F the BAT_ID line cannot be trusted: the battery is set by hand.
    v2.0.F alone reads it, once at boot (NHOS_BOARD_HAS_BATID); everything else
    stays manual."""

    def test_only_v20f_reads_the_battery_id_line(self):
        for name in ("BatteryGaugeManager.cpp", "BatteryGaugeManager.h", "BatteryProfile.cpp",
                     "BatteryProfile.h", "BoardPins.cpp", "BoardPins.h"):
            source = read(name)
            # The pre-v1.10.0 machinery stays gone.
            for banned in ("kBatteryIdAdcPin", "BatteryIdClass", "classifyBatteryIdAdcRaw",
                           "analogRead", "Pogo"):
                self.assertNotIn(banned, source, f"{banned} in {name}")
        board = read("BoardConfig.h")
        v15f = board[board.index("#if defined(NHOS_BOARD_V15F)"):board.index("#elif defined(NHOS_BOARD_V20F)")]
        v20f = board[board.index("#elif defined(NHOS_BOARD_V20F)"):board.index("#elif defined(NHOS_BOARD_GCU_V21_LTS)")]
        self.assertNotIn("NHOS_BOARD_HAS_BATID 1", v15f)
        self.assertIn("NHOS_BOARD_HAS_BATID 1", v20f)
        self.assertIn("#define NHOS_BOARD_HAS_BATID 0", board)
        # The one read is gated on the capability and happens in boot, before Wi-Fi.
        sketch = read("newhorizons_os.ino")
        gate = sketch.index("#if NHOS_BOARD_HAS_BATID")
        self.assertIn("readBatteryIdMv", sketch[gate:gate + 400])
        self.assertLess(gate, sketch.index("scanner.begin()"))
        self.assertEqual(sketch.count("readBatteryIdMv"), 1)
        self.assertIn("analogRead", read("BatteryIdReader.cpp"))

    def test_detect_command_is_gone(self):
        self.assertNotIn("detect_battery_profile", command_names())
        self.assertNotIn("detectNow", read("BatteryGaugeManager.h"))

    def test_profile_resolves_from_the_manual_setting_only(self):
        header = read("BatteryProfile.h")
        self.assertIn("BatteryProfile resolveBatteryProfile(const ManualBatteryProfile& manualProfile);", header)
        self.assertIn("kUnconfiguredBatteryChargeLimitMa = 100;", header)
        gauge = read("BatteryGaugeManager.cpp")
        self.assertEqual(gauge.count("resolveBatteryProfile(manualProfile_, batteryId_)"), 3)
        # A gauge sample no longer re-decides the battery.
        apply = gauge[gauge.index("void BatteryGaugeManager::applySample"):]
        apply = apply[:apply.index("\n}\n")]
        self.assertNotIn("updateProfile", apply)

    def test_status_keeps_the_fields_older_desktops_read(self):
        gauge = read("BatteryGaugeManager.cpp")
        self.assertIn('profile_.automatic ? "auto" : "manual"', gauge)
        self.assertIn('\\"profile_source\\":\\""', gauge)
        self.assertIn('\\"profile_resolved\\":', gauge)
        self.assertIn('\\"profile_required\\":', gauge)
        # The ID is reported only where it is read.
        block = gauge[gauge.index('\\"battery_id\\"') - 80:gauge.index('\\"battery_id\\"')]
        self.assertIn("#if NHOS_BOARD_HAS_BATID", block)

    def test_charge_profiles_never_exceed_the_battery_maximum(self):
        power = read("PowerManager.cpp")
        apply = power[power.index("bool PowerManager::applyProfile("):]
        apply = apply[:apply.index("\n}\n")]
        self.assertIn("effectiveChargeCurrentMa(config.chargeCurrentMa, chargeCeilingMa_)", apply)
        self.assertIn("writeRegister(kBq25180IchgCtrlRegister, ichgCtrl)", apply)
        # A profile write must not silently resume a paused charge.
        self.assertIn("ichg | (chargingPaused_ ? kBq25180ChargeDisableBit : 0)", apply)
        self.assertNotIn("applyBatteryChargeLimit", power + read("PowerManager.h"))
        self.assertIn("power_->setChargeCeilingMa(profile_.maxChargeCurrentMa, actualMa)",
                      read("BatteryGaugeManager.cpp"))


if __name__ == "__main__":
    unittest.main()
