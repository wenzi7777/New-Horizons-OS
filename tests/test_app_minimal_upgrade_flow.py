import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
RECOVERY_APP_PATH = REPO_ROOT / "device" / "recovery" / "recovery_app.py"


def load_recovery_app_module():
    fake_machine = types.SimpleNamespace(reset=lambda: None)
    fake_iconfig = types.SimpleNamespace(
        FIRMWARE_NAME="New Horizons OS",
        LOG_PATH="device_state/logs/device.log",
        DEVICE_STATE_DIR="device_state",
        DEFAULT_CONTROL_PORT=22345,
        STATUS_ANNOUNCE_INTERVAL_MS=2000,
        DEFAULT_MANIFESTS={
            "recovery": "https://example.com/recovery/manifest.json",
            "os": "https://example.com/os/manifest.json",
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
    fake_wifi_mod = types.SimpleNamespace(WiFiManager=lambda *args, **kwargs: None)
    fake_os_writer_mod = types.SimpleNamespace(
        OSWriter=lambda *args, **kwargs: types.SimpleNamespace(
            check_os_release=lambda release_url: {
                "status": "ok",
                "message": "os_release_checked",
                "latest_version": "v0.2.0",
                "manifest_url": "https://example.com/os-manifest.json",
            },
            write_os=lambda release_url: {
                "status": "ok",
                "message": "os_write_complete",
                "downloaded_files": 1,
                "skipped_files": 2,
                "reboot_required": True,
            },
        )
    )

    injected = {
        "machine": fake_machine,
        "immutable_config": fake_iconfig,
        "device_identity": fake_identity,
        "device_logging": fake_logger_mod,
        "filesystem_api": fake_fs_mod,
        "mqtt_transport": fake_mqtt_mod,
        "runtime_config": fake_runtime_mod,
        "udp_control": fake_udp_mod,
        "wifi_manager": fake_wifi_mod,
        "os_writer": fake_os_writer_mod,
    }
    old_modules = {}
    for name, module in injected.items():
        old_modules[name] = sys.modules.get(name)
        sys.modules[name] = module

    try:
        spec = importlib.util.spec_from_file_location("recovery_app_test_module", RECOVERY_APP_PATH)
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


class RecoveryOSWriterFlowTests(unittest.TestCase):
    def test_upgrade_to_full_is_not_supported(self):
        module = load_recovery_app_module()
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.runtime = {
            "mode": "recovery",
            "update": {
                "manifest_url": "https://example.com/recovery/manifest.json",
                "enabled": True,
            },
        }
        app.config_store = FakeConfigStore(app.runtime)
        app.reboot_required = False
        app.reboot_deadline_ms = None

        result = app._handle_request({"command": "upgrade_to_full"}, None)

        self.assertEqual(result["status"], "error")
        self.assertEqual(result["message"], "unknown_command")
        self.assertEqual(result["error"], "upgrade_to_full")

    def test_write_os_runs_in_recovery_and_uses_release_url(self):
        module = load_recovery_app_module()
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.runtime = {
            "mode": "recovery",
            "update": {
                "release_url": "https://example.com/latest.json",
                "enabled": True,
            },
        }
        app.config_store = FakeConfigStore(app.runtime)
        app.os_writer = None
        app.logger = None

        result = app._handle_request({"command": "write_os"}, None)

        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["message"], "os_write_complete")
        self.assertEqual(result["release_url"], "https://example.com/latest.json")


if __name__ == "__main__":
    unittest.main()
