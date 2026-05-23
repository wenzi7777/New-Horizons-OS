import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_runtime_module(module_path, module_name, config_name, config_module, storage_module=None):
    old_config = sys.modules.get(config_name)
    old_storage = sys.modules.get("storage")
    storage_module = storage_module or types.SimpleNamespace(load_tlv=lambda *args: {}, save_tlv=lambda *args: None)
    sys.modules[config_name] = config_module
    sys.modules["storage"] = storage_module
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
            PACKET_VERSION=2,
            TARGET_FPS=60,
            SEND_EVERY_N_FRAMES=1,
            MATRIX_SETTLE_US=20,
            DEFAULT_SERVER_HOST="",
            DEFAULT_UDP_STREAM_PORT=13250,
            DEFAULT_GATEWAY_DISCOVERY_PORT=22346,
            GITHUB_BASE_URL="https://example.com/device",
            RECOVERY_GITHUB_BASE_URL="https://example.com/base-device",
            GITHUB_RELEASE_URL="https://example.com/releases/latest.tlv",
            DEFAULT_RELEASE_URL="https://example.com/releases/latest.tlv",
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
        self.assertEqual(module.DEFAULT_RUNTIME["indicators"]["oled"]["mode"], "off")
        self.assertEqual(module.DEFAULT_RUNTIME["indicators"]["external_led"]["mode"], "off")
        self.assertNotIn("enabled", module.DEFAULT_RUNTIME["indicators"]["oled"])
        self.assertNotIn("enabled", module.DEFAULT_RUNTIME["indicators"]["external_led"])
        self.assertEqual(module.DEFAULT_RUNTIME["transport"]["mode"], "udp")
        self.assertEqual(module.DEFAULT_RUNTIME["update"]["release_url"], "https://example.com/releases/latest.tlv")
        self.assertEqual(module.DEFAULT_RUNTIME["update"]["source"], "github")
        self.assertEqual(sorted(module.DEFAULT_RUNTIME["update"]["sources"].keys()), ["github"])
        self.assertEqual(
            module.DEFAULT_RUNTIME["update"]["sources"]["github"]["os"],
            "https://example.com/device/os/manifest.tlv",
        )
        self.assertEqual(
            module.DEFAULT_RUNTIME["update"]["sources"]["github"]["recovery"],
            "https://example.com/base-device/recovery/manifest.tlv",
        )
        store = module.RuntimeConfigStore("device_state")
        self.assertEqual(store.runtime_path, "device_state/runtime_config.tlv")
        self.assertEqual(store.network_path, "device_state/network_config.tlv")
        self.assertEqual(store.filter_path, "device_state/filter_config.tlv")
        self.assertEqual(store.update_state_path, "device_state/update_state.tlv")

    def test_os_runtime_loads_do_not_write_merged_defaults(self):
        saves = []
        fake_storage = types.SimpleNamespace(
            load_tlv=lambda *args: {},
            save_tlv=lambda path, payload: saves.append((path, payload)),
        )
        fake_config = types.SimpleNamespace(
            PACKET_VERSION=2,
            TARGET_FPS=60,
            SEND_EVERY_N_FRAMES=1,
            MATRIX_SETTLE_US=20,
            DEFAULT_SERVER_HOST="",
            DEFAULT_UDP_STREAM_PORT=13250,
            DEFAULT_GATEWAY_DISCOVERY_PORT=22346,
            GITHUB_BASE_URL="https://example.com/device",
            GITHUB_RELEASE_URL="https://example.com/releases/latest.tlv",
            DEFAULT_RELEASE_URL="https://example.com/releases/latest.tlv",
            WIFI_MODE="STA",
        )
        module = load_runtime_module(
            REPO_ROOT / "device" / "os" / "runtime_config.py",
            "os_runtime_no_default_write_test",
            "config",
            fake_config,
            fake_storage,
        )
        store = module.RuntimeConfigStore("device_state")

        store.load_runtime()
        store.load_network()
        store.load_filter()

        self.assertEqual(saves, [])

        store.update_runtime({"mode": "normal"})
        store.update_network({"ssid": "lab"})
        store.update_filter({"enabled": True})

        self.assertEqual(len(saves), 3)

    def test_recovery_runtime_defaults_enable_default_logging(self):
        fake_iconfig = types.SimpleNamespace(
            FIRMWARE_NAME="New Horizons OS",
            DEFAULT_MODE="recovery",
            DEFAULT_BUFFER_FRAMES=8,
            DEFAULT_NTP_SERVERS=["pool.ntp.org"],
            DEFAULT_TARGET_FPS=60,
            DEFAULT_SERVER_HOST="",
            DEFAULT_UDP_STREAM_PORT=13250,
            DEFAULT_GATEWAY_DISCOVERY_PORT=22346,
            DEFAULT_MANIFESTS={"recovery": "recovery.tlv", "os": "os.tlv"},
            DEFAULT_RELEASE_URL="https://example.com/latest.tlv",
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
        self.assertEqual(module.DEFAULT_RUNTIME["transport"]["mode"], "udp")
        self.assertEqual(module.DEFAULT_RUNTIME["update"]["release_url"], "https://example.com/latest.tlv")
        self.assertEqual(module.DEFAULT_RUNTIME["update"]["source"], "github")
        self.assertEqual(sorted(module.DEFAULT_RUNTIME["update"]["sources"].keys()), ["github"])

    def test_recovery_runtime_loads_do_not_write_merged_defaults(self):
        saves = []
        fake_storage = types.SimpleNamespace(
            load_tlv=lambda *args: {},
            save_tlv=lambda path, payload: saves.append((path, payload)),
        )
        fake_iconfig = types.SimpleNamespace(
            FIRMWARE_NAME="New Horizons OS",
            DEFAULT_MODE="recovery",
            DEFAULT_BUFFER_FRAMES=8,
            DEFAULT_NTP_SERVERS=["pool.ntp.org"],
            DEFAULT_TARGET_FPS=60,
            DEFAULT_SERVER_HOST="",
            DEFAULT_UDP_STREAM_PORT=13250,
            DEFAULT_GATEWAY_DISCOVERY_PORT=22346,
            DEFAULT_MANIFESTS={"recovery": "recovery.tlv", "os": "os.tlv"},
            DEFAULT_RELEASE_URL="https://example.com/latest.tlv",
            DEVICE_STATE_DIR="device_state",
        )
        module = load_runtime_module(
            REPO_ROOT / "device" / "recovery" / "runtime_config.py",
            "recovery_runtime_no_default_write_test",
            "immutable_config",
            fake_iconfig,
            fake_storage,
        )
        store = module.RuntimeConfigStore("device_state")
        self.assertEqual(store.runtime_path, "device_state/runtime_config.tlv")
        self.assertEqual(store.network_path, "device_state/network_config.tlv")
        self.assertEqual(store.filter_path, "device_state/filter_config.tlv")
        self.assertEqual(store.update_state_path, "device_state/update_state.tlv")

        store.load_runtime()
        store.load_network()
        store.load_filter()

        self.assertEqual(saves, [])

        store.update_runtime({"mode": "recovery"})
        store.update_network({"ssid": "lab"})

        self.assertEqual(len(saves), 2)


if __name__ == "__main__":
    unittest.main()
