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
            "ap_ssid": "NewHorizonsOS-010203040506",
            "saved_ssid": "LabWiFi",
            "state": "wifi_setup_active",
            "last_error": "",
            "last_setup_result": "",
            "server": {"host": "", "tcp_port": 22345, "udp_port": 13250, "source": "findme", "gateway_id": ""},
            "findme": {
                "enabled": True,
                "port": 22346,
                "state": "idle",
                "gateway_id": "",
                "host": "",
                "tcp_port": 22345,
                "udp_port": 13250,
                "last_success_ms": 0,
                "last_error": "",
                "source": "findme",
            },
            "transport": {"mode": "udp_tcp"},
            "mode": "normal",
            "os_installed": True,
            "release_url": GITHUB_RELEASE_URL,
            "versions": {
                "runtime": "v0.2.17",
                "recovery": "v0.2.20",
                "os": "v0.2.18",
            },
        }

    def scan_networks(self):
        return [{"ssid": "LabWiFi", "security": "wpa2-psk", "rssi": -45}]

    def apply_credentials(self, ssid, password, release_url="", log_enabled="", log_capacity=""):
        self.calls.append((ssid, password, release_url, log_enabled, log_capacity))
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


class WiFiPortalGatewayDiscoveryTests(unittest.TestCase):
    def _style_block(self, html):
        match = re.search(r"<style>(.*?)</style>", html, re.S)
        self.assertIsNotNone(match)
        return match.group(1)

    def test_index_page_only_renders_wifi_setup_and_gateway_status(self):
        module = load_portal_module()
        portal = module.WiFiSetupPortal(FakeManager(), FakeConfig(), None)

        html = portal._render_index_page()

        self.assertIn('name="ssid"', html)
        self.assertIn('name="password"', html)
        self.assertIn("Gateway: auto discovery on this LAN", html)
        self.assertIn("Discovery:", html)
        self.assertNotIn('name="server_profile"', html)
        self.assertNotIn('id="developer_options"', html)
        self.assertNotIn('data-developer="1"', html)
        self.assertNotIn(">Manual</option>", html)
        self.assertNotIn("isensing-s1.u-aizu.ac.jp", html)
        self.assertNotIn('name="server_host"', html)
        self.assertNotIn('name="tcp_port"', html)
        self.assertNotIn('name="udp_port"', html)
        self.assertNotIn('name="release_url"', html)
        self.assertNotIn('id="release_url"', html)
        self.assertNotIn('name="mqtt_tls"', html)
        self.assertNotIn('name="log_enabled"', html)
        self.assertNotIn('name="log_capacity"', html)
        self.assertNotIn('name="transport_mode"', html)

    def test_connect_form_shows_apply_overlay_on_submit(self):
        module = load_portal_module()
        portal = module.WiFiSetupPortal(FakeManager(), FakeConfig(), None)

        html = portal._render_index_page()

        self.assertIn('onsubmit="showApplyOverlay();"', html)
        self.assertIn('id="apply_overlay"', html)
        self.assertIn("Applying settings<br>Do not touch the device power switch.", html)
        self.assertNotIn("正在套用設定", html)
        self.assertNotIn("請不要觸碰", html)
        self.assertIn("function showApplyOverlay()", html)

    def test_connect_post_still_reports_handled_when_response_aborts_after_apply(self):
        module = load_portal_module()
        manager = FakeManager()
        portal = module.WiFiSetupPortal(manager, FakeConfig(), None)
        portal.active = True
        portal.server = FakeServer(FakeClient())
        portal._read_request = lambda client: ("POST", "/connect", "ssid=LabWiFi&password=secret")
        portal._send_response = lambda client, status, content_type, body: (_ for _ in ()).throw(OSError("ECONNABORTED"))

        handled = portal.service()

        self.assertTrue(handled)
        self.assertEqual(manager.calls[0][:2], ("LabWiFi", "secret"))

    def test_connect_post_forwards_wifi_only_to_manager(self):
        module = load_portal_module()
        manager = FakeManager()
        portal = module.WiFiSetupPortal(manager, FakeConfig(), None)
        portal.active = True
        portal.server = FakeServer(FakeClient())
        portal._read_request = lambda client: ("POST", "/connect", "ssid=LabWiFi&password=secret")
        portal._send_response = lambda client, status, content_type, body: None

        handled = portal.service()

        self.assertTrue(handled)
        self.assertEqual(manager.calls, [("LabWiFi", "secret", "", "", "")])

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
        self.assertIn("This device is in recovery mode.", html)
        self.assertNotIn("Recovery device detected", html)
        self.assertIn("New Horizons OS must be written", html)

    def test_index_page_omits_system_version_footer(self):
        module = load_portal_module()
        portal = module.WiFiSetupPortal(FakeManager(), FakeConfig(), None)

        html = portal._render_index_page()

        self.assertNotIn("Runtime version", html)
        self.assertNotIn("Recovery version", html)
        self.assertNotIn("OS version", html)

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

    def test_gateway_missing_result_has_clear_title(self):
        module = load_portal_module()
        portal = module.WiFiSetupPortal(FakeManager(), FakeConfig(), None)

        html = portal._render_result_page({
            "ok": False,
            "wifi_connected": True,
            "message": "Wi-Fi connected, but no New Horizons Gateway was discovered.",
        })

        self.assertIn("Gateway not discovered", html)
        self.assertIn("Wi-Fi connected, but no New Horizons Gateway was discovered.", html)


if __name__ == "__main__":
    unittest.main()
