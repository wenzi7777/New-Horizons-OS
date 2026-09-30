import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_ROOT = REPO_ROOT / "firmware" / "newhorizons_os"


def read(name: str) -> str:
    return (FIRMWARE_ROOT / name).read_text(encoding="utf-8")


class RebootWifiSetupTests(unittest.TestCase):
    """`reboot_wifi_setup`: the boot button's setup portal, without the button."""

    def test_command_sets_the_flag_then_reboots(self):
        control = read("ControlServer.cpp")
        start = control.index('if (cmd == "reboot_wifi_setup")')
        body = control[start:control.index("\n  }\n", start)]
        self.assertLess(body.index("requestWifiSetupOnNextBoot()"), body.index("requestReboot()"))
        self.assertIn('return ok(cmd, "wifi_setup_reboot_scheduled");', body)

    def test_flag_is_one_shot_and_feeds_the_same_setup_path_as_the_button(self):
        boot = read("BootModeManager.cpp")
        begin = boot[boot.index("void BootModeManager::begin()"):]
        begin = begin[:begin.index("\n}\n")]
        read_at = begin.index('prefs_.getBool("setup_req", false)')
        self.assertLess(read_at, begin.index('prefs_.remove("setup_req");'))
        self.assertIn("wifiSetupRequested_ = setupCommanded || sampleWifiSetupButtonWindow();", begin)
        self.assertIn("wifiSetupRequested_ = setupCommanded || sampleMultiCycleSetupTrigger();", begin)
        self.assertIn('prefs_.putBool("setup_req", true);', boot)

    def test_espnow_devices_fall_back_to_the_portal_too(self):
        sketch = read("newhorizons_os.ino")
        self.assertIn("} else if (espNowMode && bootMode.wifiSetupRequested()) {", sketch)


if __name__ == "__main__":
    unittest.main()
