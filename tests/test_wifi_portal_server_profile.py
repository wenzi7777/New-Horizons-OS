import importlib.util
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
PORTAL_PATH = REPO_ROOT / "device" / "immutable" / "wifi_portal.py"


def load_portal_module():
    spec = importlib.util.spec_from_file_location("wifi_portal_test_module", PORTAL_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec is not None and spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FakeManager:
    def __init__(self):
        self.calls = []

    def portal_status(self):
        return {
            "portal_ip": "192.168.4.1",
            "portal_url": "http://newhorizons.os",
            "portal_ip_url": "http://192.168.4.1",
            "portal_domain": "newhorizons.os",
            "ap_ssid": "NewHorizonsOS-010203",
            "saved_ssid": "LabWiFi",
            "state": "wifi_setup_active",
            "last_error": "",
            "last_setup_result": "",
            "server_profile": "production",
            "server_profile_options": [
                {
                    "value": "local",
                    "label": "本地版",
                    "master_host": "192.168.1.153",
                    "data_host": "192.168.1.153",
                },
                {
                    "value": "production",
                    "label": "正式版",
                    "master_host": "isensing-s1.u-aizu.ac.jp",
                    "data_host": "isensing-s1.u-aizu.ac.jp",
                },
            ],
            "master_server": {"host": "isensing-s1.u-aizu.ac.jp", "port": 22345},
            "data_server": {"host": "isensing-s1.u-aizu.ac.jp", "port": 5005},
        }

    def scan_networks(self):
        return [{"ssid": "LabWiFi", "security": "wpa2-psk", "rssi": -45}]

    def apply_credentials(self, ssid, password, server_profile=None):
        self.calls.append((ssid, password, server_profile))
        return {"ok": True, "message": "Connected"}


class FakeConfig:
    SETUP_PORTAL_TITLE = "New Horizons OS Wi-Fi Setup"
    SETUP_PORTAL_HOST = "192.168.4.1"
    SETUP_PORTAL_PORT = 80
    SETUP_AP_SSID_PREFIX = "NewHorizonsOS"


class FakeClient:
    def settimeout(self, _timeout):
        return None

    def close(self):
        return None


class FakeServer:
    def __init__(self, client):
        self.client = client
        self.accepted = False

    def accept(self):
        if self.accepted:
            raise OSError("done")
        self.accepted = True
        return self.client, ("127.0.0.1", 54321)


class WiFiPortalServerProfileTests(unittest.TestCase):
    def test_index_page_renders_server_profile_selector(self):
        module = load_portal_module()
        portal = module.WiFiSetupPortal(FakeManager(), FakeConfig(), None)

        html = portal._render_index_page()

        self.assertIn('name="server_profile"', html)
        self.assertIn("正式版 (isensing-s1.u-aizu.ac.jp)", html)
        self.assertIn("本地版 (192.168.1.153)", html)
        self.assertIn("isensing-s1.u-aizu.ac.jp", html)

    def test_connect_post_forwards_server_profile_to_manager(self):
        module = load_portal_module()
        manager = FakeManager()
        portal = module.WiFiSetupPortal(manager, FakeConfig(), None)
        portal.active = True
        portal.server = FakeServer(FakeClient())
        portal._read_request = lambda client: (
            "POST",
            "/connect",
            "ssid=LabWiFi&password=secret&server_profile=production",
        )
        portal._send_response = lambda client, status, content_type, body: None

        handled = portal.service()

        self.assertTrue(handled)
        self.assertEqual(manager.calls, [("LabWiFi", "secret", "production")])


if __name__ == "__main__":
    unittest.main()
