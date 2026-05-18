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
            "master_server": {"host": "192.168.1.153", "port": 22345},
            "data_server": {"host": "192.168.1.153", "port": 5005},
            "mqtt": {"host": "isensing-s1.u-aizu.ac.jp", "port": 8883, "tls": True},
            "transport": {"mode": "udp", "topic_namespace": "newhorizons/v1"},
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
            "master_server": dict(self.runtime.get("master_server", {})),
            "data_server": dict(self.runtime.get("data_server", {})),
            "mqtt": dict(self.runtime.get("mqtt", {})),
            "transport": dict(self.runtime.get("transport", {})),
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


def load_wifi_manager(channel):
    path = REPO_ROOT / "device" / "channels" / channel / "files" / "wifi_manager.py"
    fake_network = FakeNetwork()
    fake_config = types.SimpleNamespace(
        WIFI_MODE="STA",
        SETUP_AP_PASSWORD="newhorizons",
        SETUP_AP_SSID_PREFIX="NewHorizonsOS",
        SETUP_PORTAL_DOMAIN="newhorizons.os",
        SETUP_PORTAL_HOST="192.168.4.1",
        SETUP_PORTAL_PORT=80,
        PRINT_WIFI_STATUS=True,
        DEFAULT_SERVER_PROFILE="manual",
        SERVER_PROFILES={
            "manual": {
                "label": "Manual",
                "master_server": {"host": "192.168.1.153", "port": 22345},
                "data_server": {"host": "192.168.1.153", "port": 5005},
            },
            "production": {
                "label": "Production",
                "master_server": {"host": "isensing-s1.u-aizu.ac.jp", "port": 22345},
                "data_server": {"host": "isensing-s1.u-aizu.ac.jp", "port": 5005},
            },
        },
    )
    fake_secrets = types.SimpleNamespace(WIFI_SSID="", WIFI_PASSWORD="")
    fake_portal = types.SimpleNamespace(WiFiSetupPortal=FakePortal)
    fake_identity = types.SimpleNamespace(get_device_suffix=lambda: "010203")
    fake_time = types.SimpleNamespace(sleep_ms=lambda _ms: None, sleep=lambda _s: None)
    fake_gc = types.SimpleNamespace(collect=lambda: None, mem_free=lambda: 65536)

    saved_modules = {}
    for name, module in {
        "network": fake_network,
        "config": fake_config,
        "secrets": fake_secrets,
        "wifi_portal": fake_portal,
        "device_identity": fake_identity,
        "time": fake_time,
        "gc": fake_gc,
    }.items():
        saved_modules[name] = sys.modules.get(name)
        sys.modules[name] = module

    try:
        spec = importlib.util.spec_from_file_location(f"wifi_manager_{channel}", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:
        for name, saved_module in saved_modules.items():
            if saved_module is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = saved_module

    return module, fake_network


class WiFiManagerApStartTests(unittest.TestCase):
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

    def test_portal_status_prefers_friendly_domain(self):
        module, fake_network = load_wifi_manager("minimal")

        manager = module.WiFiManager()
        manager.start_setup_portal()
        status = manager.portal_status()

        self.assertEqual(status["portal_domain"], "newhorizons.os")
        self.assertEqual(status["portal_url"], "http://newhorizons.os")
        self.assertEqual(status["portal_ip_url"], "http://192.168.4.1")

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

        result = manager.apply_credentials("CampusWiFi", "pw", "production")

        self.assertTrue(result["ok"])
        self.assertEqual(store.runtime["server_profile"], "production")
        self.assertEqual(store.runtime["master_server"]["host"], "isensing-s1.u-aizu.ac.jp")
        self.assertEqual(store.runtime["master_server"]["port"], 22345)
        self.assertEqual(store.runtime["data_server"]["host"], "isensing-s1.u-aizu.ac.jp")
        self.assertEqual(store.runtime["data_server"]["port"], 5005)

    def test_apply_credentials_with_manual_profile_updates_runtime_endpoints(self):
        module, _fake_network = load_wifi_manager("minimal")
        store = FakeConfigStore()
        manager = module.WiFiManager(config_store=store)
        manager.start_setup_portal()

        result = manager.apply_credentials(
            "CampusWiFi",
            "pw",
            "manual",
            master_host="192.168.1.200",
            master_port="32001",
            data_host="192.168.1.201",
            data_port="32002",
            mqtt_host="192.168.1.153",
            mqtt_port="1883",
            mqtt_tls="false",
            transport_mode="mqtt",
        )

        self.assertTrue(result["ok"])
        self.assertEqual(store.runtime["server_profile"], "manual")
        self.assertEqual(store.runtime["master_server"]["host"], "192.168.1.200")
        self.assertEqual(store.runtime["master_server"]["port"], 32001)
        self.assertEqual(store.runtime["data_server"]["host"], "192.168.1.201")
        self.assertEqual(store.runtime["data_server"]["port"], 32002)
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
            master_host="192.168.1.210",
            master_port="32101",
            data_host="192.168.1.211",
            data_port="32102",
            mqtt_host="192.168.1.154",
            mqtt_port="1884",
            mqtt_tls="false",
            transport_mode="mqtt",
        )

        self.assertTrue(result["ok"])
        self.assertEqual(store.runtime["server_profile"], "manual")
        self.assertEqual(store.runtime["master_server"]["host"], "192.168.1.210")
        self.assertEqual(store.runtime["master_server"]["port"], 32101)
        self.assertEqual(store.runtime["data_server"]["host"], "192.168.1.211")
        self.assertEqual(store.runtime["data_server"]["port"], 32102)
        self.assertEqual(store.runtime["mqtt"], {"host": "192.168.1.154", "port": 1884, "tls": False})
        self.assertEqual(store.runtime["transport"]["mode"], "mqtt")

    def test_portal_status_reports_selected_server_profile(self):
        module, _fake_network = load_wifi_manager("minimal")
        store = FakeConfigStore()
        store.update_runtime({
            "server_profile": "production",
            "master_server": {"host": "isensing-s1.u-aizu.ac.jp", "port": 22345},
            "data_server": {"host": "isensing-s1.u-aizu.ac.jp", "port": 5005},
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
                    "master_host": "192.168.1.153",
                    "data_host": "192.168.1.153",
                },
                {
                    "value": "production",
                    "label": "Production",
                    "master_host": "isensing-s1.u-aizu.ac.jp",
                    "data_host": "isensing-s1.u-aizu.ac.jp",
                },
            ],
        )


if __name__ == "__main__":
    unittest.main()
