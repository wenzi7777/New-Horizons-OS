import importlib.util
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
PORTAL_PATH = REPO_ROOT / "device" / "recovery" / "wifi_portal.py"
GITHUB_RELEASE_URL = "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.json"


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
            "server_profile": "",
            "server_profile_options": [
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
            "master_server": {"host": "isensing-s1.u-aizu.ac.jp", "port": 22345},
            "data_server": {"host": "isensing-s1.u-aizu.ac.jp", "port": 5005},
            "mqtt": {"host": "isensing-s1.u-aizu.ac.jp", "port": 8883, "tls": True},
            "transport": {"mode": "mqtt"},
            "mode": "normal",
            "os_installed": True,
            "release_url": GITHUB_RELEASE_URL,
        }

    def scan_networks(self):
        return [{"ssid": "LabWiFi", "security": "wpa2-psk", "rssi": -45}]

    def apply_credentials(
        self,
        ssid,
        password,
        server_profile=None,
        master_host="",
        master_port="",
        data_host="",
        data_port="",
        mqtt_host="",
        mqtt_port="",
        mqtt_tls="",
        transport_mode="",
        release_url="",
    ):
        self.calls.append(
            (
                ssid,
                password,
                server_profile,
                master_host,
                master_port,
                data_host,
                data_port,
                mqtt_host,
                mqtt_port,
                mqtt_tls,
                transport_mode,
                release_url,
            )
        )
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
    def _style_block(self, html):
        match = re.search(r"<style>(.*?)</style>", html, re.S)
        self.assertIsNotNone(match)
        return match.group(1)

    def test_index_page_renders_server_profile_selector(self):
        module = load_portal_module()
        portal = module.WiFiSetupPortal(FakeManager(), FakeConfig(), None)

        html = portal._render_index_page()

        self.assertIn('name="server_profile"', html)
        self.assertIn('value="production" selected', html)
        self.assertIn("Production (isensing-s1.u-aizu.ac.jp)", html)
        self.assertIn('id="developer_options"', html)
        self.assertIn('data-developer="1"', html)
        self.assertIn(">Manual</option>", html)
        self.assertNotIn("Manual (192.168.1.153)", html)
        self.assertIn('name="master_host"', html)
        self.assertIn('name="master_port"', html)
        self.assertIn('name="data_host"', html)
        self.assertIn('name="data_port"', html)
        self.assertIn('name="mqtt_host"', html)
        self.assertIn('name="mqtt_port"', html)
        self.assertIn(GITHUB_RELEASE_URL, html)
        self.assertNotIn('name="release_url"', html)
        self.assertNotIn('id="release_url"', html)
        self.assertNotIn('name="mqtt_tls"', html)
        self.assertNotIn('name="log_enabled"', html)
        self.assertNotIn('name="log_capacity"', html)
        self.assertNotIn('name="transport_mode"', html)
        self.assertNotIn(">UDP<", html)
        self.assertNotIn("Production uses", html)
        self.assertNotIn("Transport: MQTT", html)
        self.assertNotIn("placeholder=\"e.g. 192.168.1.153\"", html)
        self.assertIn("isensing-s1.u-aizu.ac.jp", html)

    def test_connect_form_shows_apply_overlay_on_submit(self):
        module = load_portal_module()
        portal = module.WiFiSetupPortal(FakeManager(), FakeConfig(), None)

        html = portal._render_index_page()

        self.assertIn('onsubmit="showApplyOverlay();"', html)
        self.assertIn('id="apply_overlay"', html)
        self.assertIn("配置正在套用 請不要觸碰電源開關。", html)
        self.assertIn("function showApplyOverlay()", html)

    def test_production_page_keeps_manual_defaults_out_of_option_label(self):
        module = load_portal_module()
        portal = module.WiFiSetupPortal(FakeManager(), FakeConfig(), None)

        html = portal._render_index_page()

        self.assertIn('value="192.168.1.153"', html)
        self.assertNotIn("Manual (192.168.1.153)", html)

    def test_connect_post_forwards_server_profile_to_manager(self):
        module = load_portal_module()
        manager = FakeManager()
        portal = module.WiFiSetupPortal(manager, FakeConfig(), None)
        portal.active = True
        portal.server = FakeServer(FakeClient())
        portal._read_request = lambda client: (
            "POST",
            "/connect",
            "ssid=LabWiFi&password=secret&server_profile=manual&master_host=192.168.1.200&master_port=32001&data_host=192.168.1.201&data_port=32002&mqtt_host=192.168.1.153&mqtt_port=1883",
        )
        portal._send_response = lambda client, status, content_type, body: None

        handled = portal.service()

        self.assertTrue(handled)
        self.assertEqual(
            manager.calls,
            [
                (
                    "LabWiFi",
                    "secret",
                    "manual",
                    "192.168.1.200",
                    "32001",
                    "192.168.1.201",
                    "32002",
                    "192.168.1.153",
                    "1883",
                    "",
                    "",
                    "",
                )
            ],
        )

    def test_recovery_page_prompts_os_write(self):
        module = load_portal_module()
        manager = FakeManager()
        status = manager.portal_status()
        status["mode"] = "recovery"
        status["os_installed"] = False
        manager.portal_status = lambda: status
        portal = module.WiFiSetupPortal(manager, FakeConfig(), None)

        html = portal._render_index_page()

        self.assertIn("Recovery Mode", html)
        self.assertIn("Recovery device detected", html)
        self.assertIn("New Horizons OS must be written", html)

    def test_index_page_uses_compact_embedded_styles(self):
        module = load_portal_module()
        portal = module.WiFiSetupPortal(FakeManager(), FakeConfig(), None)

        css = self._style_block(portal._render_index_page())

        self.assertLess(len(css), 1900)
        self.assertNotIn("gradient", css)
        self.assertNotIn("box-shadow", css)
        self.assertNotIn("@media", css)

    def test_result_page_uses_compact_embedded_styles(self):
        module = load_portal_module()
        portal = module.WiFiSetupPortal(FakeManager(), FakeConfig(), None)

        css = self._style_block(portal._render_result_page({"ok": True, "message": "Connected"}))

        self.assertLess(len(css), 700)
        self.assertNotIn("box-shadow", css)


if __name__ == "__main__":
    unittest.main()
