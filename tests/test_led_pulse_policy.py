import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SK6812_PATH = REPO_ROOT / "device" / "os" / "sk6812.py"


class FakeNeoPixel(list):
    def __init__(self, pin, count):
        super().__init__([(0, 0, 0)] * count)
        self.pin = pin
        self.count = count
        self.write_calls = 0

    def write(self):
        self.write_calls += 1


class FakeI2C:
    def __init__(self, addresses=()):
        self.addresses = list(addresses)
        self.scan_calls = 0
        self.writes = []

    def scan(self):
        self.scan_calls += 1
        return list(self.addresses)

    def writeto(self, addr, payload):
        self.writes.append((addr, bytes(payload)))


class FakeFrameBuffer:
    def __init__(self, buffer, width, height, mode):
        self.buffer = buffer
        self.width = width
        self.height = height
        self.mode = mode

    def fill(self, value):
        for idx in range(len(self.buffer)):
            self.buffer[idx] = value

    def text(self, *_args):
        return None


def load_sk6812_module(i2c_addresses=()):
    saved_modules = {}
    fake_i2c = FakeI2C(i2c_addresses)
    fake_neopixel_calls = []

    def fake_neopixel(pin, count):
        np = FakeNeoPixel(pin, count)
        fake_neopixel_calls.append((pin, count, np))
        return np

    injected = {
        "machine": types.SimpleNamespace(Pin=lambda pin: pin),
        "neopixel": types.SimpleNamespace(NeoPixel=fake_neopixel),
        "board_pins": types.SimpleNamespace(
            SK6812_PIN=1,
            SK6812_COUNT=1,
            WS2812B_PIN=2,
            WS2812B_COUNT=3,
            SSD1306_ADDR_PRIMARY=0x3C,
            SSD1306_ADDR_FALLBACK=0x3D,
            SSD1306_WIDTH=128,
            SSD1306_HEIGHT=32,
        ),
        "i2c_bus": types.SimpleNamespace(get_i2c=lambda: fake_i2c),
        "framebuf": types.SimpleNamespace(FrameBuffer=FakeFrameBuffer, MONO_VLSB=0),
    }
    for name, module in injected.items():
        saved_modules[name] = sys.modules.get(name)
        sys.modules[name] = module
    try:
        spec = importlib.util.spec_from_file_location("sk6812_policy_test", SK6812_PATH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module._test_fake_i2c = fake_i2c
        module._test_fake_neopixel_calls = fake_neopixel_calls
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

    def test_begin_only_starts_onboard_led_and_leaves_optional_indicators_unloaded(self):
        module, saved_modules = load_sk6812_module(i2c_addresses=[0x3C])
        try:
            led = module.SK6812Status()
            led.begin()

            self.assertEqual(len(module._test_fake_neopixel_calls), 1)
            self.assertEqual(module._test_fake_neopixel_calls[0][1], 1)
            self.assertEqual(module._test_fake_i2c.scan_calls, 0)
            self.assertIsNone(led.external_np)
            self.assertIsNone(led.oled_i2c)
            self.assertIsNone(led.oled_buffer)
            self.assertEqual(led.status()["external_led"]["mode"], "off")
            self.assertEqual(led.status()["oled"]["mode"], "off")
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

    def test_oled_auto_without_device_releases_resources(self):
        module, saved_modules = load_sk6812_module(i2c_addresses=[])
        try:
            led = module.SK6812Status()
            led.configure({"oled": {"mode": "auto"}})
            self.assertFalse(led.ensure_optional_indicators())

            status = led.status()["oled"]
            self.assertEqual(status["mode"], "auto")
            self.assertFalse(status["active"])
            self.assertFalse(status["detected"])
            self.assertEqual(status["last_error"], "not_detected")
            self.assertIsNone(led.oled_i2c)
            self.assertIsNone(led.oled_fb)
            self.assertIsNone(led.oled_buffer)
            self.assertIsNone(led.oled_framebuf)
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

    def test_oled_on_without_device_reports_inactive_but_keeps_setting(self):
        module, saved_modules = load_sk6812_module(i2c_addresses=[])
        try:
            led = module.SK6812Status()
            led.configure({"oled": {"mode": "on"}})
            led.ensure_optional_indicators()

            status = led.status()["oled"]
            self.assertEqual(status["mode"], "on")
            self.assertFalse(status["active"])
            self.assertEqual(status["last_error"], "not_detected")
            self.assertIsNone(led.oled_buffer)
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

    def test_external_led_on_loads_only_during_optional_stage_and_release_frees_it(self):
        module, saved_modules = load_sk6812_module()
        try:
            led = module.SK6812Status()
            led.begin()
            led.configure({"external_led": {"mode": "on"}})

            self.assertEqual(len(module._test_fake_neopixel_calls), 1)
            self.assertIsNone(led.external_np)
            led.ensure_optional_indicators()
            self.assertEqual(len(module._test_fake_neopixel_calls), 2)
            self.assertIsNotNone(led.external_np)

            led.configure({"external_led": {"mode": "off"}})
            self.assertIsNone(led.external_np)
            self.assertIsNone(led.external_last_colors)
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
