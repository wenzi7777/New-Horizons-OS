import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_runtime_module(module_path, module_name, config_name, config_module):
    old_config = sys.modules.get(config_name)
    old_storage = sys.modules.get("storage")
    sys.modules[config_name] = config_module
    sys.modules["storage"] = types.SimpleNamespace(load_json=lambda *args: {}, save_json=lambda *args: None)
    try:
        spec = importlib.util.spec_from_file_location(module_name, module_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        if old_config is None:
            sys.modules.pop(config_name, None)
        else:
            sys.modules[config_name] = old_config
        if old_storage is None:
            sys.modules.pop("storage", None)
        else:
            sys.modules["storage"] = old_storage


class LoggingRuntimeConfigTests(unittest.TestCase):
    def test_os_runtime_defaults_enable_default_logging(self):
        fake_config = types.SimpleNamespace(
            SERVER_PROFILES={},
            DEFAULT_SERVER_PROFILE="",
            UDP_SERVER_IP="127.0.0.1",
            UDP_CONTROL_PORT=22345,
            UDP_SERVER_PORT=5005,
            PACKET_VERSION=1,
            TARGET_FPS=60,
            SEND_EVERY_N_FRAMES=1,
            MATRIX_SETTLE_US=20,
            MQTT_TOPIC_NAMESPACE="newhorizons/v1",
            MQTT_BROKER_HOST="127.0.0.1",
            MQTT_BROKER_PORT=1883,
            MQTT_TLS=False,
            MQTT_USERNAME="",
            MQTT_PASSWORD="",
            GITHUB_BASE_URL="https://example.com/device",
            GITHUB_RELEASE_URL="https://example.com/releases/latest.json",
            DEFAULT_RELEASE_URL="https://example.com/releases/latest.json",
            WIFI_MODE="STA",
        )
        module = load_runtime_module(
            REPO_ROOT / "device" / "os" / "runtime_config.py",
            "os_runtime_logging_test",
            "config",
            fake_config,
        )

        self.assertEqual(
            module.DEFAULT_RUNTIME["logging"],
            {"enabled": True, "capacity": "default", "serial": "status"},
        )
        self.assertEqual(module.DEFAULT_RUNTIME["transport"]["mode"], "mqtt")
        self.assertEqual(module.DEFAULT_RUNTIME["update"]["release_url"], "https://example.com/releases/latest.json")
        self.assertEqual(module.DEFAULT_RUNTIME["update"]["source"], "github")
        self.assertEqual(sorted(module.DEFAULT_RUNTIME["update"]["sources"].keys()), ["github"])

    def test_recovery_runtime_defaults_enable_default_logging(self):
        fake_iconfig = types.SimpleNamespace(
            FIRMWARE_NAME="New Horizons OS",
            DEFAULT_MODE="recovery",
            SERVER_PROFILES={},
            DEFAULT_SERVER_PROFILE="",
            DEFAULT_MASTER_HOST="127.0.0.1",
            DEFAULT_MASTER_PORT=22345,
            DEFAULT_DATA_HOST="127.0.0.1",
            DEFAULT_DATA_PORT=5005,
            DEFAULT_BUFFER_FRAMES=8,
            DEFAULT_NTP_SERVERS=["pool.ntp.org"],
            DEFAULT_TARGET_FPS=60,
            DEFAULT_TOPIC_NAMESPACE="newhorizons/v1",
            DEFAULT_MQTT_HOST="127.0.0.1",
            DEFAULT_MQTT_PORT=1883,
            DEFAULT_MQTT_TLS=False,
            DEFAULT_MQTT_USERNAME="",
            DEFAULT_MQTT_PASSWORD="",
            DEFAULT_MANIFESTS={"recovery": "recovery.json", "os": "os.json"},
            DEFAULT_RELEASE_URL="https://example.com/latest.json",
            DEVICE_STATE_DIR="device_state",
        )
        module = load_runtime_module(
            REPO_ROOT / "device" / "recovery" / "runtime_config.py",
            "recovery_runtime_logging_test",
            "immutable_config",
            fake_iconfig,
        )

        self.assertEqual(
            module.DEFAULT_RUNTIME["logging"],
            {"enabled": True, "capacity": "default", "serial": "status"},
        )
        self.assertEqual(module.DEFAULT_RUNTIME["transport"]["mode"], "mqtt")
        self.assertEqual(module.DEFAULT_RUNTIME["update"]["release_url"], "https://example.com/latest.json")
        self.assertEqual(module.DEFAULT_RUNTIME["update"]["source"], "github")
        self.assertEqual(sorted(module.DEFAULT_RUNTIME["update"]["sources"].keys()), ["github"])


if __name__ == "__main__":
    unittest.main()
