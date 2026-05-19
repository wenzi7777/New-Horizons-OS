import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


class FakeAP:
    def __init__(self):
        self.enabled = False
        self.calls = []

    def active(self, value=None):
        if value is None:
            return self.enabled
        self.calls.append(("active", value))
        self.enabled = bool(value)
        return self.enabled

    def config(self, *args, **kwargs):
        if args == ("mac",):
            return b"\xaa\xbb\xcc\x01\x02\x03"
        self.calls.append(("config", kwargs))
        if not self.enabled:
            raise OSError("Wifi Invalid Mode")

    def ifconfig(self):
        return ("192.168.4.1", "255.255.255.0", "192.168.4.1", "0.0.0.0")


class FakeSTA:
    def __init__(self):
        self.enabled = False
        self.connected = False
        self.calls = []

    def active(self, value=None):
        if value is None:
            return self.enabled
        self.calls.append(("active", value))
        self.enabled = bool(value)
        return self.enabled

    def isconnected(self):
        return self.connected

    def disconnect(self):
        self.calls.append(("disconnect", None))

    def connect(self, ssid, password):
        self.calls.append(("connect", ssid, password))
        self.connected = True

    def ifconfig(self):
        return ("192.168.1.50", "255.255.255.0", "192.168.1.1", "8.8.8.8")

    def config(self, key):
        if key == "mac":
            return b"\xaa\xbb\xcc\x01\x02\x03"
        return None


class FakeNetwork(types.SimpleNamespace):
    AP_IF = 1
    STA_IF = 0

    def __init__(self):
        super().__init__()
        self.ap = FakeAP()
        self.sta = FakeSTA()

    def WLAN(self, iface):
        return self.ap if iface == self.AP_IF else self.sta


class FakePortal:
    def __init__(self, *args, **kwargs):
        self.active = False

    def start(self):
        self.active = True

    def stop(self):
        self.active = False


class FakeConfigStore:
    def __init__(self):
        self.network = {
            "wifi_mode": "STA",
            "ssid": "",
            "password": "",
            "setup_method": "softap_webui",
            "last_ssid": "",
        }
        self.runtime = {
            "server_profile": "manual",
            "mqtt": {"host": "192.168.1.153", "port": 1883, "tls": False},
            "transport": {"mode": "mqtt", "topic_namespace": "newhorizons/v1"},
            "update": {
                "release_url": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.json",
                "source": "github",
            },
        }

    def load_network(self):
        return dict(self.network)

    def save_network(self, network):
        self.network = dict(network)

    def update_network(self, patch):
        self.network.update(patch)
        return dict(self.network)

    def load_runtime(self):
        return {
            "server_profile": self.runtime.get("server_profile", "manual"),
            "mqtt": dict(self.runtime.get("mqtt", {})),
            "transport": dict(self.runtime.get("transport", {})),
            "update": dict(self.runtime.get("update", {})),
        }

    def update_runtime(self, patch):
        for key, value in patch.items():
            if isinstance(value, dict) and isinstance(self.runtime.get(key), dict):
                merged = dict(self.runtime[key])
                merged.update(value)
                self.runtime[key] = merged
            else:
                self.runtime[key] = value
        return self.load_runtime()


def load_wifi_manager(channel, include_os_dir=True):
    target_dir = "recovery" if channel == "minimal" else "os"
    path = REPO_ROOT / "device" / target_dir / "wifi_manager.py"
    fake_network = FakeNetwork()
    config_values = dict(
        WIFI_MODE="STA",
        SETUP_AP_PASSWORD="newhorizons",
        SETUP_AP_SSID_PREFIX="NewHorizonsOS",
        SETUP_PORTAL_DOMAIN="newhorizons.os",
        SETUP_PORTAL_HOST="192.168.4.1",
        SETUP_PORTAL_PORT=80,
        PRINT_WIFI_STATUS=True,
        MQTT_BROKER_HOST="192.168.1.153",
        MQTT_BROKER_PORT=1883,
        MQTT_TLS=False,
        PRODUCTION_MQTT_HOST="isensing-s1.u-aizu.ac.jp",
        PRODUCTION_MQTT_PORT=8883,
        PRODUCTION_MQTT_TLS=True,
        DEFAULT_RELEASE_URL="https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.json",
        DEFAULT_SERVER_PROFILE="manual",
        SERVER_PROFILES={
            "manual": {
                "label": "Manual",
                "mqtt": {"host": "192.168.1.153", "port": 1883, "tls": False},
            },
            "production": {
                "label": "Production",
                "mqtt": {"host": "isensing-s1.u-aizu.ac.jp", "port": 8883, "tls": True},
            },
        },
    )
    if include_os_dir:
        config_values["OS_DIR"] = "nhos"
    fake_config = types.SimpleNamespace(**config_values)
    fake_secrets = types.SimpleNamespace(WIFI_SSID="", WIFI_PASSWORD="")
    portal_instances = []

    class CountingPortal(FakePortal):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            portal_instances.append(self)

    fake_portal = types.SimpleNamespace(WiFiSetupPortal=CountingPortal)
    fake_identity = types.SimpleNamespace(get_device_suffix=lambda: "010203")
    fake_time = types.SimpleNamespace(sleep_ms=lambda _ms: None, sleep=lambda _s: None)
    fake_gc = types.SimpleNamespace(collect=lambda: None, mem_free=lambda: 65536)
    fake_storage = types.SimpleNamespace(exists=lambda _path: False)

    saved_modules = {}
    for name, module in {
        "network": fake_network,
        "config": fake_config,
        "secrets": fake_secrets,
        "wifi_portal": fake_portal,
        "device_identity": fake_identity,
        "time": fake_time,
        "gc": fake_gc,
        "storage": fake_storage,
    }.items():
        saved_modules[name] = sys.modules.get(name)
        sys.modules[name] = module

    try:
        spec = importlib.util.spec_from_file_location(f"wifi_manager_{channel}", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.WiFiSetupPortal = CountingPortal
        module._test_portal_instances = portal_instances
    finally:
        for name, saved_module in saved_modules.items():
            if saved_module is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = saved_module

    return module, fake_network


class WiFiManagerApStartTests(unittest.TestCase):
    def test_minimal_channel_does_not_allocate_ap_during_normal_sta_boot(self):
        module, fake_network = load_wifi_manager("minimal")
        manager = module.WiFiManager()

        ok = manager.connect_sta("TestWiFi", "pw")

        self.assertTrue(ok)
        self.assertEqual(fake_network.ap.calls, [])

    def test_full_channel_does_not_allocate_ap_during_normal_sta_boot(self):
        module, fake_network = load_wifi_manager("full")
        manager = module.WiFiManager()

        ok = manager.connect_sta("TestWiFi", "pw")

        self.assertTrue(ok)
        self.assertEqual(fake_network.ap.calls, [])

    def test_full_channel_does_not_construct_portal_during_normal_sta_boot(self):
        module, _fake_network = load_wifi_manager("full")
        manager = module.WiFiManager()

        ok = manager.connect_sta("TestWiFi", "pw")

        self.assertTrue(ok)
        self.assertEqual(module._test_portal_instances, [])

    def test_minimal_channel_activates_ap_before_config(self):
        module, fake_network = load_wifi_manager("minimal")

        module.WiFiManager().start_setup_portal()

        self.assertEqual(fake_network.ap.calls[0], ("active", False))
        self.assertEqual(fake_network.ap.calls[1], ("active", True))
        self.assertEqual(fake_network.ap.calls[2][0], "config")

    def test_full_channel_activates_ap_before_config(self):
        module, fake_network = load_wifi_manager("full")

        module.WiFiManager().start_setup_portal()

        self.assertEqual(fake_network.ap.calls[0], ("active", False))
        self.assertEqual(fake_network.ap.calls[1], ("active", True))
        self.assertEqual(fake_network.ap.calls[2][0], "config")

    def test_minimal_channel_starts_ap_before_constructing_portal(self):
        module, fake_network = load_wifi_manager("minimal")
        original_ensure_portal = module.WiFiManager._ensure_portal

        def tracked_ensure_portal(manager):
            fake_network.ap.calls.append(("ensure_portal", None))
            return original_ensure_portal(manager)

        module.WiFiManager._ensure_portal = tracked_ensure_portal

        module.WiFiManager().start_setup_portal()

        self.assertLess(
            fake_network.ap.calls.index(("active", True)),
            fake_network.ap.calls.index(("ensure_portal", None)),
        )
        self.assertLess(
            next(i for i, call in enumerate(fake_network.ap.calls) if call[0] == "config"),
            fake_network.ap.calls.index(("ensure_portal", None)),
        )

    def test_full_channel_starts_ap_before_constructing_portal(self):
        module, fake_network = load_wifi_manager("full")
        original_ensure_portal = module.WiFiManager._ensure_portal

        def tracked_ensure_portal(manager):
            fake_network.ap.calls.append(("ensure_portal", None))
            return original_ensure_portal(manager)

        module.WiFiManager._ensure_portal = tracked_ensure_portal

        module.WiFiManager().start_setup_portal()

        self.assertLess(
            fake_network.ap.calls.index(("active", True)),
            fake_network.ap.calls.index(("ensure_portal", None)),
        )
        self.assertLess(
            next(i for i, call in enumerate(fake_network.ap.calls) if call[0] == "config"),
            fake_network.ap.calls.index(("ensure_portal", None)),
        )

    def test_portal_status_prefers_friendly_domain(self):
        module, fake_network = load_wifi_manager("minimal")

        manager = module.WiFiManager()
        manager.start_setup_portal()
        status = manager.portal_status()

        self.assertEqual(status["portal_domain"], "newhorizons.os")
        self.assertEqual(status["portal_url"], "http://newhorizons.os")
        self.assertEqual(status["portal_ip_url"], "http://192.168.4.1")
        self.assertNotIn("versions", status)

    def test_minimal_channel_disables_ap_after_successful_sta_connect(self):
        module, fake_network = load_wifi_manager("minimal")
        manager = module.WiFiManager()
        manager.start_setup_portal()

        ok = manager.connect_sta("TestWiFi", "pw")

        self.assertTrue(ok)
        self.assertIn(("active", False), fake_network.ap.calls)
        self.assertFalse(fake_network.ap.enabled)

    def test_full_channel_disables_ap_after_successful_sta_connect(self):
        module, fake_network = load_wifi_manager("full")
        manager = module.WiFiManager()
        manager.start_setup_portal()

        ok = manager.connect_sta("TestWiFi", "pw")

        self.assertTrue(ok)
        self.assertIn(("active", False), fake_network.ap.calls)
        self.assertFalse(fake_network.ap.enabled)

    def test_apply_credentials_with_production_profile_updates_runtime_endpoints(self):
        module, _fake_network = load_wifi_manager("minimal")
        store = FakeConfigStore()
        manager = module.WiFiManager(config_store=store)
        manager.start_setup_portal()

        result = manager.apply_credentials(
            "CampusWiFi",
            "pw",
            "production",
            mqtt_host="192.168.1.153",
            mqtt_port="1883",
            mqtt_tls="false",
        )

        self.assertTrue(result["ok"])
        self.assertEqual(store.runtime["server_profile"], "production")
        self.assertEqual(store.runtime["mqtt"], {"host": "isensing-s1.u-aizu.ac.jp", "port": 8883, "tls": True})
        self.assertEqual(store.runtime["transport"]["mode"], "mqtt")

    def test_apply_credentials_with_manual_profile_updates_runtime_endpoints(self):
        module, _fake_network = load_wifi_manager("minimal")
        store = FakeConfigStore()
        manager = module.WiFiManager(config_store=store)
        manager.start_setup_portal()

        result = manager.apply_credentials(
            "CampusWiFi",
            "pw",
            "manual",
            mqtt_host="192.168.1.153",
            mqtt_port="1883",
            mqtt_tls="true",
        )

        self.assertTrue(result["ok"])
        self.assertEqual(store.runtime["server_profile"], "manual")
        self.assertEqual(store.runtime["mqtt"], {"host": "192.168.1.153", "port": 1883, "tls": False})
        self.assertEqual(store.runtime["transport"]["mode"], "mqtt")

    def test_manual_profile_without_mqtt_fields_uses_local_defaults(self):
        module, _fake_network = load_wifi_manager("minimal")
        store = FakeConfigStore()
        manager = module.WiFiManager(config_store=store)
        manager.start_setup_portal()

        result = manager.apply_credentials(
            "CampusWiFi",
            "pw",
            "manual",
        )

        self.assertTrue(result["ok"])
        self.assertEqual(store.runtime["mqtt"], {"host": "192.168.1.153", "port": 1883, "tls": False})
        self.assertEqual(store.runtime["transport"]["mode"], "mqtt")

    def test_full_channel_manual_profile_updates_runtime_ports(self):
        module, _fake_network = load_wifi_manager("full")
        store = FakeConfigStore()
        manager = module.WiFiManager(config_store=store)
        manager.start_setup_portal()

        result = manager.apply_credentials(
            "CampusWiFi",
            "pw",
            "manual",
            mqtt_host="192.168.1.154",
            mqtt_port="1884",
            mqtt_tls="false",
        )

        self.assertTrue(result["ok"])
        self.assertEqual(store.runtime["server_profile"], "manual")
        self.assertEqual(store.runtime["mqtt"], {"host": "192.168.1.154", "port": 1884, "tls": False})
        self.assertEqual(store.runtime["transport"]["mode"], "mqtt")

    def test_portal_status_reports_selected_server_profile(self):
        module, _fake_network = load_wifi_manager("minimal")
        store = FakeConfigStore()
        store.update_runtime({
            "server_profile": "production",
            "mqtt": {"host": "isensing-s1.u-aizu.ac.jp", "port": 8883, "tls": True},
        })

        manager = module.WiFiManager(config_store=store)
        status = manager.portal_status()

        self.assertEqual(status["server_profile"], "production")
        self.assertEqual(
            status["server_profile_options"],
            [
                {
                    "value": "manual",
                    "label": "Manual",
                    "mqtt_host": "192.168.1.153",
                    "mqtt_port": 1883,
                    "mqtt_tls": False,
                },
                {
                    "value": "production",
                    "label": "Production",
                    "mqtt_host": "isensing-s1.u-aizu.ac.jp",
                    "mqtt_port": 8883,
                    "mqtt_tls": True,
                },
            ],
        )

    def test_recovery_portal_status_defaults_missing_os_dir_to_nhos(self):
        module, _fake_network = load_wifi_manager("minimal", include_os_dir=False)
        checked_paths = []
        module.storage.exists = lambda path: checked_paths.append(path) or False

        status = module.WiFiManager(config_store=FakeConfigStore()).portal_status()

        self.assertFalse(status["os_installed"])
        self.assertEqual(checked_paths, ["nhos/app.mpy"])


if __name__ == "__main__":
    unittest.main()
