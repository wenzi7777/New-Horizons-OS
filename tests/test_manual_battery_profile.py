import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_ROOT = REPO_ROOT / "firmware" / "newhorizons_os"


def read(name: str) -> str:
    return (FIRMWARE_ROOT / name).read_text(encoding="utf-8")


class ManualBatteryProfileTests(unittest.TestCase):
    """The current PCB's BAT_ID cannot be trusted: the battery is set by hand."""

    def test_nothing_reads_the_battery_id_line(self):
        for name in ("BatteryGaugeManager.cpp", "BatteryGaugeManager.h", "BatteryProfile.cpp",
                     "BatteryProfile.h", "BoardPins.cpp", "BoardPins.h"):
            source = read(name)
            for banned in ("kBatteryIdAdcPin", "BatteryIdClass", "classifyBatteryIdAdcRaw",
                           "readBatteryId", "analogRead", "Pogo"):
                self.assertNotIn(banned, source, f"{banned} in {name}")

    def test_detect_command_is_gone(self):
        self.assertNotIn('cmd == "detect_battery_profile"', read("ControlServer.cpp"))
        self.assertNotIn("detectNow", read("BatteryGaugeManager.h"))

    def test_profile_resolves_from_the_manual_setting_only(self):
        header = read("BatteryProfile.h")
        self.assertIn("BatteryProfile resolveBatteryProfile(const ManualBatteryProfile& manualProfile);", header)
        self.assertIn("kUnconfiguredBatteryChargeLimitMa = 100;", header)
        gauge = read("BatteryGaugeManager.cpp")
        self.assertEqual(gauge.count("resolveBatteryProfile(manualProfile_)"), 2)
        # A gauge sample no longer re-decides the battery.
        apply = gauge[gauge.index("void BatteryGaugeManager::applySample"):]
        apply = apply[:apply.index("\n}\n")]
        self.assertNotIn("updateProfile", apply)

    def test_status_keeps_the_fields_older_desktops_read(self):
        gauge = read("BatteryGaugeManager.cpp")
        self.assertIn('\\"profile_source\\":\\"manual\\"', gauge)
        self.assertIn('\\"profile_resolved\\":', gauge)
        self.assertIn('\\"profile_required\\":', gauge)
        self.assertNotIn('\\"battery_id\\"', gauge)

    def test_charge_profiles_never_exceed_the_battery_maximum(self):
        power = read("PowerManager.cpp")
        apply = power[power.index("bool PowerManager::applyProfile("):]
        apply = apply[:apply.index("\n}\n")]
        self.assertIn("effectiveChargeCurrentMa(config.chargeCurrentMa, chargeCeilingMa_)", apply)
        self.assertIn("writeRegister(kBq25180IchgCtrlRegister, ichg)", apply)
        self.assertNotIn("applyBatteryChargeLimit", power + read("PowerManager.h"))
        self.assertIn("power_->setChargeCeilingMa(profile_.maxChargeCurrentMa, actualMa)",
                      read("BatteryGaugeManager.cpp"))


if __name__ == "__main__":
    unittest.main()
