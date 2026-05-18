import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
APP_MINIMAL_PATH = REPO_ROOT / "device" / "channels" / "minimal" / "files" / "app_minimal.py"


def load_app_minimal_module():
    fake_machine = types.SimpleNamespace(reset=lambda: None)
    fake_iconfig = types.SimpleNamespace(
        FIRMWARE_NAME="New Horizons OS",
        LOG_PATH="device_state/logs/device.log",
        DEVICE_STATE_DIR="device_state",
        DEFAULT_CONTROL_PORT=22345,
        STATUS_ANNOUNCE_INTERVAL_MS=2000,
        DEFAULT_MANIFESTS={
            "minimal": "https://example.com/minimal/manifest.json",
            "full": "https://example.com/full/manifest.json",
        },
    )
    fake_identity = types.SimpleNamespace(
        get_device_id=lambda: 0x12345678,
        get_device_uid=lambda: "UID123",
        get_device_name=lambda default: default,
    )
    fake_logger_mod = types.SimpleNamespace(DeviceLogger=lambda path: None)
    fake_fs_mod = types.SimpleNamespace(FilesystemAPI=lambda root: None)
    fake_mqtt_mod = types.SimpleNamespace(
        MQTTTransport=lambda *args, **kwargs: types.SimpleNamespace(
            poll=lambda *poll_args, **poll_kwargs: None,
            publish_status=lambda *status_args, **status_kwargs: True,
        )
    )
    fake_runtime_mod = types.SimpleNamespace(RuntimeConfigStore=lambda root: None)
    fake_udp_mod = types.SimpleNamespace(UDPControlServer=lambda port, logger=None: None)
    fake_update_mod = types.SimpleNamespace(UpdateManager=lambda *args, **kwargs: None)
    fake_wifi_mod = types.SimpleNamespace(WiFiManager=lambda *args, **kwargs: None)

    injected = {
        "machine": fake_machine,
        "immutable_config": fake_iconfig,
        "device_identity": fake_identity,
        "device_logging": fake_logger_mod,
        "filesystem_api": fake_fs_mod,
        "mqtt_transport": fake_mqtt_mod,
        "runtime_config": fake_runtime_mod,
        "udp_control": fake_udp_mod,
        "update_manager": fake_update_mod,
        "wifi_manager": fake_wifi_mod,
    }
    old_modules = {}
    for name, module in injected.items():
        old_modules[name] = sys.modules.get(name)
        sys.modules[name] = module

    try:
        spec = importlib.util.spec_from_file_location("app_minimal_test_module", APP_MINIMAL_PATH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.time.ticks_add = lambda now, delta: now + delta
        return module
    finally:
        for name, old in old_modules.items():
            if old is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = old


class FakeConfigStore:
    def __init__(self, runtime):
        self.runtime = dict(runtime)

    def load_runtime(self):
        return dict(self.runtime)

    def update_runtime(self, patch):
        merged = dict(self.runtime)
        for key, value in patch.items():
            if isinstance(value, dict) and isinstance(merged.get(key), dict):
                next_value = dict(merged[key])
                next_value.update(value)
                merged[key] = next_value
            else:
                merged[key] = value
        self.runtime = merged
        return dict(self.runtime)


class FakeUpdates:
    def __init__(self):
        self.started = 0

    def is_busy(self):
        return False

    def status(self):
        return {"phase": "idle", "reboot_required": False}

    def start_apply(self):
        self.started += 1
        return {"status": "ok", "message": "update_started", "reboot_required": False}


class MinimalUpgradeFlowTests(unittest.TestCase):
    def test_upgrade_to_full_switches_channel_only_after_apply_completes(self):
        module = load_app_minimal_module()
        app = module.MinimalApp.__new__(module.MinimalApp)
        app.runtime = {
            "channel": "minimal",
            "update": {
                "manifest_url": "https://example.com/minimal/manifest.json",
                "enabled": True,
            },
        }
        app.config_store = FakeConfigStore(app.runtime)
        app.updates = FakeUpdates()
        app.pending_channel_switch = None
        app.reboot_required = False
        app.reboot_deadline_ms = None

        result = app._handle_request({"command": "upgrade_to_full"}, None)

        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["message"], "upgrade_to_full_started")
        self.assertEqual(app.runtime["channel"], "minimal")
        self.assertEqual(
            app.runtime["update"]["manifest_url"],
            "https://example.com/full/manifest.json",
        )
        self.assertEqual(app.pending_channel_switch, "full")

        app._handle_update_result(
            {"status": "ok", "message": "update_applied", "reboot_required": True},
            5000,
        )

        self.assertEqual(app.runtime["channel"], "full")
        self.assertEqual(app.pending_channel_switch, None)
        self.assertTrue(app.reboot_required)


if __name__ == "__main__":
    unittest.main()
