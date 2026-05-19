import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SK6812_PATH = REPO_ROOT / "device" / "os" / "sk6812.py"


def load_sk6812_module():
    saved_modules = {}
    injected = {
        "machine": types.SimpleNamespace(Pin=lambda pin: pin),
        "neopixel": types.SimpleNamespace(NeoPixel=lambda pin, count: []),
        "board_pins": types.SimpleNamespace(SK6812_PIN=1, SK6812_COUNT=1),
    }
    for name, module in injected.items():
        saved_modules[name] = sys.modules.get(name)
        sys.modules[name] = module
    try:
        spec = importlib.util.spec_from_file_location("sk6812_policy_test", SK6812_PATH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module, saved_modules
    except Exception:
        for name, saved in saved_modules.items():
            if saved is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = saved
        raise


class LedPulsePolicyTests(unittest.TestCase):
    def test_normal_os_led_policy_has_all_named_states_and_safe_brightness(self):
        module, saved_modules = load_sk6812_module()
        try:
            led = module.SK6812Status()
            required_states = {
                "boot_window",
                "wifi_setup",
                "normal",
                "updating",
                "reboot_required",
                "maintenance",
                "calibration",
                "error",
                "charging",
                "charge_done",
                "off",
            }
            self.assertTrue(required_states.issubset(set(led.state_map)))

            for state, data in led.state_map.items():
                brightness = data["brightness"]
                if state == "off":
                    self.assertEqual(brightness, 0.0)
                else:
                    self.assertGreaterEqual(brightness, 0.10, state)
                    self.assertLessEqual(brightness, 0.50, state)
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

    def test_charge_done_uses_miku_green_and_charging_is_not_red(self):
        module, saved_modules = load_sk6812_module()
        try:
            led = module.SK6812Status()
            self.assertEqual(led.state_map["charge_done"]["colors"][0], (57, 197, 187))

            charging_color = led.state_map["charging"]["colors"][0]
            self.assertGreater(charging_color[1], 0)
            self.assertNotEqual(charging_color[1:], (0, 0))
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

    def test_no_power_off_states_are_near_solid_warning_patterns(self):
        module, saved_modules = load_sk6812_module()
        try:
            led = module.SK6812Status()
            for state in ("updating", "reboot_required"):
                data = led.state_map[state]
                on_ms = sum(
                    interval
                    for interval, color in zip(data["intervals"], data["colors"])
                    if color != (0, 0, 0)
                )
                total_ms = sum(data["intervals"])
                self.assertGreaterEqual(on_ms / total_ms, 0.8, state)
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved


if __name__ == "__main__":
    unittest.main()
