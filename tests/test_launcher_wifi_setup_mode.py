import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
LAUNCHER_PATH = REPO_ROOT / "device" / "immutable" / "launcher.py"


def load_launcher_module(injected_modules):
    saved_modules = {}
    for name, module in injected_modules.items():
        saved_modules[name] = sys.modules.get(name)
        sys.modules[name] = module

    try:
        spec = importlib.util.spec_from_file_location("launcher_test_module", LAUNCHER_PATH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.time.ticks_ms = lambda: 0
        module.time.ticks_diff = lambda now, then: now - then
        module.time.sleep_ms = lambda _ms: None
        return module, saved_modules
    except Exception:
        for name, saved in saved_modules.items():
            if saved is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = saved
        raise


class FakeLogger:
    def __init__(self, path):
        self.path = path
        self.messages = []

    def info(self, message):
        self.messages.append(("info", message))

    def warn(self, message):
        self.messages.append(("warn", message))

    def error(self, message):
        self.messages.append(("error", message))


class FakePinFactory:
    IN = 0
    PULL_UP = 0

    def __call__(self, *args, **kwargs):
        return types.SimpleNamespace(value=lambda: 0)


class LauncherWifiSetupModeTests(unittest.TestCase):
    def test_wifi_setup_request_keeps_full_channel_when_runtime_channel_is_full(self):
        minimal_calls = []
        full_calls = []
        fake_logger = FakeLogger("device_state/logs/device.log")

        injected = {
            "machine": types.SimpleNamespace(Pin=FakePinFactory()),
            "immutable_config": types.SimpleNamespace(
                ACTION_BUTTON_PIN=46,
                BOOT_WINDOW_MS=3000,
                BOOT_WINDOW_POLL_MS=50,
                DEFAULT_CHANNEL="minimal",
                DEVICE_STATE_DIR="device_state",
                LOG_PATH="device_state/logs/device.log",
            ),
            "device_logging": types.SimpleNamespace(DeviceLogger=lambda path: fake_logger),
            "runtime_config": types.SimpleNamespace(
                RuntimeConfigStore=lambda base_dir: types.SimpleNamespace(
                    load_runtime=lambda: {"channel": "full"}
                )
            ),
            "app_minimal": types.SimpleNamespace(
                run=lambda wifi_setup_requested=False, recovery_error="": minimal_calls.append(
                    (wifi_setup_requested, recovery_error)
                )
            ),
            "app": types.SimpleNamespace(
                App=lambda wifi_setup_requested=False: types.SimpleNamespace(
                    run=lambda: full_calls.append(wifi_setup_requested)
                )
            ),
            "recovery": types.SimpleNamespace(run=lambda **kwargs: None),
        }
        module, saved_modules = load_launcher_module(injected)
        try:
            module.run()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(minimal_calls, [])
        self.assertEqual(full_calls, [True])
        self.assertNotIn(("warn", "launcher_force_minimal_for_wifi_setup"), fake_logger.messages)


if __name__ == "__main__":
    unittest.main()
