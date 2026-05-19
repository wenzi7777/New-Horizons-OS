import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
LAUNCHER_PATH = REPO_ROOT / "device" / "root" / "launcher.py"


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
    def test_missing_os_dir_defaults_to_nhos_and_starts_recovery(self):
        recovery_calls = []
        checked_paths = []
        fake_logger = FakeLogger("device_state/logs/device.log")

        injected = {
            "machine": types.SimpleNamespace(Pin=FakePinFactory()),
            "immutable_config": types.SimpleNamespace(
                ACTION_BUTTON_PIN=46,
                BOOT_WINDOW_MS=3000,
                BOOT_WINDOW_POLL_MS=50,
                DEFAULT_MODE="recovery",
                DEVICE_STATE_DIR="device_state",
                LOG_PATH="device_state/logs/device.log",
            ),
            "device_logging": types.SimpleNamespace(DeviceLogger=lambda path: fake_logger),
            "runtime_config": types.SimpleNamespace(
                RuntimeConfigStore=lambda base_dir: types.SimpleNamespace(load_runtime=lambda: {"mode": "recovery"})
            ),
            "storage": types.SimpleNamespace(
                exists=lambda path: checked_paths.append(path) or False,
            ),
            "recovery_app": types.SimpleNamespace(
                run=lambda wifi_setup_requested=False, recovery_error="": recovery_calls.append(
                    (wifi_setup_requested, recovery_error)
                )
            ),
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

        self.assertEqual(checked_paths, ["nhos/app.py", "nhos/main.py"])
        self.assertEqual(recovery_calls, [(True, "")])

    def test_launcher_fallback_calls_recovery_app_without_recovery_wrapper(self):
        recovery_calls = []
        fake_logger = FakeLogger("device_state/logs/device.log")

        def failing_exists(path):
            raise RuntimeError("missing os dir")

        injected = {
            "machine": types.SimpleNamespace(Pin=FakePinFactory()),
            "immutable_config": types.SimpleNamespace(
                ACTION_BUTTON_PIN=46,
                BOOT_WINDOW_MS=3000,
                BOOT_WINDOW_POLL_MS=50,
                DEFAULT_MODE="recovery",
                DEVICE_STATE_DIR="device_state",
                LOG_PATH="device_state/logs/device.log",
                OS_DIR="nhos",
            ),
            "device_logging": types.SimpleNamespace(DeviceLogger=lambda path: fake_logger),
            "runtime_config": types.SimpleNamespace(
                RuntimeConfigStore=lambda base_dir: types.SimpleNamespace(load_runtime=lambda: {"mode": "recovery"})
            ),
            "storage": types.SimpleNamespace(exists=failing_exists),
            "recovery_app": types.SimpleNamespace(
                run=lambda wifi_setup_requested=False, recovery_error="": recovery_calls.append(
                    (wifi_setup_requested, recovery_error)
                )
            ),
            "recovery": types.SimpleNamespace(),
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

        self.assertEqual(recovery_calls, [(True, "missing os dir")])
        self.assertIn(("error", "launcher_fallback missing os dir"), fake_logger.messages)

    def test_wifi_setup_request_keeps_normal_mode_when_runtime_mode_is_normal(self):
        minimal_calls = []
        full_calls = []
        fake_logger = FakeLogger("device_state/logs/device.log")

        injected = {
            "machine": types.SimpleNamespace(Pin=FakePinFactory()),
            "immutable_config": types.SimpleNamespace(
                ACTION_BUTTON_PIN=46,
                BOOT_WINDOW_MS=3000,
                BOOT_WINDOW_POLL_MS=50,
                DEFAULT_MODE="recovery",
                DEVICE_STATE_DIR="device_state",
                LOG_PATH="device_state/logs/device.log",
                OS_DIR="nhos",
            ),
            "device_logging": types.SimpleNamespace(DeviceLogger=lambda path: fake_logger),
            "runtime_config": types.SimpleNamespace(
                RuntimeConfigStore=lambda base_dir: types.SimpleNamespace(
                    load_runtime=lambda: {"mode": "normal"}
                )
            ),
            "storage": types.SimpleNamespace(exists=lambda path: path == "nhos/app.py"),
            "recovery_app": types.SimpleNamespace(
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

    def test_normal_os_failure_schedules_recovery_reset_instead_of_inline_fallback(self):
        recovery_calls = []
        runtime_updates = []
        reset_calls = []
        fake_logger = FakeLogger("device_state/logs/device.log")

        class FakeStore:
            def load_runtime(self):
                return {"mode": "normal"}

            def update_runtime(self, updates):
                runtime_updates.append(updates)
                return {"mode": updates.get("mode", "normal"), "boot_request": updates.get("boot_request", "")}

        class FailingApp:
            def __init__(self, wifi_setup_requested=False):
                self.wifi_setup_requested = wifi_setup_requested

            def run(self):
                raise OSError("WiFi Out of Memory")

        injected = {
            "machine": types.SimpleNamespace(Pin=FakePinFactory(), reset=lambda: reset_calls.append(True)),
            "immutable_config": types.SimpleNamespace(
                ACTION_BUTTON_PIN=46,
                BOOT_WINDOW_MS=3000,
                BOOT_WINDOW_POLL_MS=50,
                DEFAULT_MODE="recovery",
                DEVICE_STATE_DIR="device_state",
                LOG_PATH="device_state/logs/device.log",
                OS_DIR="nhos",
            ),
            "device_logging": types.SimpleNamespace(DeviceLogger=lambda path: fake_logger),
            "runtime_config": types.SimpleNamespace(RuntimeConfigStore=lambda base_dir: FakeStore()),
            "storage": types.SimpleNamespace(exists=lambda path: path == "nhos/app.py"),
            "recovery_app": types.SimpleNamespace(
                run=lambda wifi_setup_requested=False, recovery_error="": recovery_calls.append(
                    (wifi_setup_requested, recovery_error)
                )
            ),
            "app": types.SimpleNamespace(App=FailingApp),
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

        self.assertEqual(recovery_calls, [])
        self.assertEqual(runtime_updates, [{"mode": "recovery", "boot_request": "recovery"}])
        self.assertEqual(reset_calls, [True])
        self.assertIn(("error", "launcher_os_failed WiFi Out of Memory"), fake_logger.messages)


if __name__ == "__main__":
    unittest.main()
