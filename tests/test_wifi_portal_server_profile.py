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
            "server_profile": "",
            "server_profile_options": [
                {
                    "value": "manual",
                    "label": "Manual",
                    "server_host": "192.168.1.153",
                    "tcp_port": 22345,
                    "udp_port": 13250,
                },
                {
                    "value": "production",
                    "label": "Production",
                    "server_host": "isensing-s1.u-aizu.ac.jp",
                    "tcp_port": 22345,
                    "udp_port": 13250,
                },
            ],
            "server": {"host": "isensing-s1.u-aizu.ac.jp", "tcp_port": 22345, "udp_port": 13250},
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

    def apply_credentials(
        self,
        ssid,
        password,
        server_profile=None,
        server_host="",
        tcp_port="",
        udp_port="",
        release_url="",
        log_enabled="",
        log_capacity="",
    ):
        self.calls.append(
            (
                ssid,
                password,
                server_profile,
                server_host,
                tcp_port,
                udp_port,
                release_url,
                log_enabled,
                log_capacity,
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
        self.assertNotIn('name="master_host"', html)
        self.assertNotIn('name="master_port"', html)
        self.assertNotIn('name="data_host"', html)
        self.assertNotIn('name="data_port"', html)
        self.assertIn('name="server_host"', html)
        self.assertIn('name="tcp_port"', html)
        self.assertIn('name="udp_port"', html)
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
        portal._read_request = lambda client: (
            "POST",
            "/connect",
            "ssid=LabWiFi&password=secret&server_profile=manual&server_host=192.168.1.153&tcp_port=22345&udp_port=13250",
        )
        portal._send_response = lambda client, status, content_type, body: (_ for _ in ()).throw(OSError("ECONNABORTED"))

        handled = portal.service()

        self.assertTrue(handled)
        self.assertEqual(manager.calls[0][2], "manual")
        self.assertEqual(manager.calls[0][3], "192.168.1.153")

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
            "ssid=LabWiFi&password=secret&server_profile=manual&server_host=192.168.1.153&tcp_port=22345&udp_port=13250",
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
                    "192.168.1.153",
                    "22345",
                    "13250",
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


if __name__ == "__main__":
    unittest.main()
