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
        HARDWARE_MODEL="VD-CTL/R v1.0.F 2026.4",
        RUNTIME_VERSION="v-runtime-test",
        RECOVERY_VERSION="v-recovery-test",
        FIRMWARE_VERSION="v-recovery-test",
        LOG_PATH="device_state/logs/device.log",
        DEVICE_STATE_DIR="device_state",
        STATUS_ANNOUNCE_INTERVAL_MS=2000,
        DEFAULT_MANIFESTS={
            "recovery": "https://example.com/recovery/manifest.json",
            "os": "https://example.com/os/manifest.json",
        },
        DEFAULT_RELEASE_URL="https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.json",
    )
    fake_identity = types.SimpleNamespace(
        get_device_id=lambda: "AABBCCDDEEFF",
        get_device_uid=lambda: "UID123",
        get_packet_device_uid_bytes=lambda: b"\xaa\xbb\xcc\xdd\xee\xff",
        get_device_name=lambda default: default,
    )
    fake_logger_mod = types.SimpleNamespace(DeviceLogger=lambda path: None)
    fake_fs_mod = types.SimpleNamespace(FilesystemAPI=lambda *args, **kwargs: None)
    fake_tcp_mod = types.SimpleNamespace(
        TCPControlTransport=lambda *args, **kwargs: types.SimpleNamespace(
            poll=lambda *poll_args, **poll_kwargs: None,
            publish_status=lambda *status_args, **status_kwargs: True,
            reconfigure=lambda: None,
            close=lambda: None,
        )
    )
    fake_runtime_mod = types.SimpleNamespace(RuntimeConfigStore=lambda root: None)
    class ForbiddenRecoveryUdpModule(types.ModuleType):
        def __getattr__(self, name):
            raise AssertionError("recovery must not import udp_control")

    fake_udp_mod = ForbiddenRecoveryUdpModule("udp_control")
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
        "tcp_control": fake_tcp_mod,
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
        module.time.ticks_ms = lambda: 1000
        module.time.ticks_diff = lambda now, then: now - then
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
        self.network = {"ssid": "saved", "password": "pw"}

    def load_runtime(self):
        return dict(self.runtime)

    def load_network(self):
        return dict(self.network)

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


class FakeLogger:
    def __init__(self):
        self.infos = []
        self.warns = []
        self.configured = []

    def info(self, message):
        self.infos.append(message)

    def warn(self, message):
        self.warns.append(message)

    def configure(self, enabled=True, capacity="default"):
        self.configured.append((enabled, capacity))


class FakeTCPControlTransport:
    def __init__(self):
        self.reconfigure_calls = 0
        self.close_calls = 0
        self.status_payloads = []
        self.progress_payloads = []
        self.flush_calls = []

    def reconfigure(self):
        self.reconfigure_calls += 1

    def close(self):
        self.close_calls += 1

    def publish_status(self, payload, wifi_connected):
        self.status_payloads.append((payload, wifi_connected))
        return True

    def publish_update_progress(self, payload, wifi_connected):
        self.progress_payloads.append((payload, wifi_connected))
        return True

    def flush(self, max_bytes=None):
        self.flush_calls.append(max_bytes)
        return True


class FakePortalWiFi:
    def __init__(self, handled=True):
        self.handled = handled

    def service_setup_portal(self):
        return self.handled


class RecoveryOSWriterFlowTests(unittest.TestCase):
    def test_recovery_app_loads_os_writer_lazily(self):
        module = load_recovery_app_module()

        self.assertIsNone(module.OSWriter)

    def test_os_write_progress_updates_state_and_flushes_compact_progress(self):
        module = load_recovery_app_module()
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.device_id = "UID123"
        app.device_uid = "UID123"
        app.device_name = "New Horizons OS"
        app.runtime = {
            "mode": "recovery",
            "transport": {"mode": "udp_tcp"},
        }
        app.wifi = type("FakeWiFi", (), {
            "is_connected": lambda self: True,
            "portal_status": lambda self: {"active": False},
        })()
        app.control_transport = FakeTCPControlTransport()
        app.control = None
        app.recovery_error = ""
        app.reboot_required = False
        app.logger = FakeLogger()
        app.last_status_announce_ms = 0

        app._os_write_progress({
            "message": "os_write_progress",
            "phase": "file_done",
            "version": "v0.2.8",
            "total_files": 4,
            "download_files": 3,
            "skipped_files": 1,
            "written_files": 2,
            "current_file": "app.py",
        })

        self.assertEqual(app.update_state["phase"], "downloading")
        self.assertEqual(app.update_state["operation"], "write_os")
        self.assertEqual(app.update_state["total_files"], 4)
        self.assertEqual(app.update_state["applied_files"], 3)
        self.assertEqual(app.update_state["downloaded_files"], 2)
        self.assertEqual(app.update_state["current_file"], "app.py")
        self.assertEqual(app.control_transport.status_payloads, [])
        self.assertEqual(len(app.control_transport.progress_payloads), 1)
        published, wifi_connected = app.control_transport.progress_payloads[0]
        self.assertTrue(wifi_connected)
        self.assertEqual(published["message"], "os_write_progress")
        self.assertEqual(published["mode"], "recovery")
        self.assertEqual(published["device_uid"], "UID123")
        self.assertEqual(published["update_state"]["applied_files"], 3)
        self.assertNotIn("runtime", published)
        self.assertEqual(app.control_transport.flush_calls, [4096])

    def test_write_os_failure_reports_error_state_without_dropping_control_session(self):
        module = load_recovery_app_module()

        class FailingOSWriter:
            def __init__(self, *args, **kwargs):
                pass

            def write_os(self, release_url):
                raise OSError("download_failed")

        module.OSWriter = FailingOSWriter
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.device_id = "UID123"
        app.device_uid = "UID123"
        app.device_name = "New Horizons OS"
        app.runtime = {
            "mode": "recovery",
            "transport": {"mode": "udp_tcp"},
            "update": {"enabled": True},
        }
        app.config_store = FakeConfigStore(app.runtime)
        app.os_writer = None
        app.logger = None
        app.control_transport = FakeTCPControlTransport()
        app.wifi = type("FakeWiFi", (), {
            "is_connected": lambda self: True,
            "release_setup_portal": lambda self: None,
            "portal_status": lambda self: {"active": False},
        })()
        app.recovery_error = ""
        app.reboot_required = False

        result = app._handle_request({"command": "write_os"}, None)

        self.assertEqual(result["status"], "error")
        self.assertEqual(result["message"], "os_write_failed")
        self.assertEqual(result["error"], "download_failed")
        self.assertEqual(result["update_state"]["phase"], "error")
        self.assertEqual(result["update_state"]["operation"], "write_os")
        self.assertEqual(result["update_state"]["last_error"], "download_failed")
        self.assertEqual(len(app.control_transport.progress_payloads), 1)
        self.assertEqual(app.control_transport.progress_payloads[0][0]["update_state"]["phase"], "error")
        self.assertEqual(app.control_transport.flush_calls, [4096])

    def test_recovery_os_installed_detects_app_entrypoint(self):
        module = load_recovery_app_module()
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        existing_paths = {"nhos/app.mpy"}
        app._path_exists = lambda path: path in existing_paths

        self.assertTrue(app._os_installed())

    def test_recovery_os_installed_ignores_python_entrypoints(self):
        module = load_recovery_app_module()
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        existing_paths = {"nhos/app.py", "nhos/main.py"}
        app._path_exists = lambda path: path in existing_paths

        self.assertFalse(app._os_installed())

    def test_recovery_status_includes_system_versions(self):
        module = load_recovery_app_module()
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        module.gc.mem_free = lambda: 120000
        module.gc.mem_alloc = lambda: 56000
        app.device_id = "UID123"
        app.device_uid = "UID123"
        app.device_name = "New Horizons OS"
        app.runtime = {
            "mode": "recovery",
            "update": {"manifest_url": "https://example.com/os-manifest.json"},
        }
        app.config_store = FakeConfigStore(app.runtime)
        app.wifi = type("FakeWiFi", (), {
            "is_connected": lambda self: True,
            "portal_status": lambda self: {"active": False},
        })()
        app.update_state = app._default_update_state()
        app.recovery_error = ""
        app.reboot_required = False
        app._path_exists = lambda path: path == "nhos/app.mpy"
        app.logger = None

        result = app._handle_request({"command": "status"}, None)

        self.assertEqual(result["system"]["name"], "New Horizons OS")
        self.assertEqual(result["system"]["mode"], "recovery")
        self.assertEqual(result["system"]["hardware_model"], "VD-CTL/R v1.0.F 2026.4")
        self.assertEqual(result["system"]["runtime_version"], "v-runtime-test")
        self.assertEqual(result["system"]["recovery_version"], "v-recovery-test")
        self.assertEqual(result["system"]["os_installed"], True)
        self.assertEqual(result["memory"]["heap_free"], 120000)
        self.assertEqual(result["memory"]["heap_allocated"], 56000)
        self.assertEqual(result["memory"]["heap_total"], 176000)

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

    def test_recovery_rejects_expired_boot_command_without_rebooting(self):
        module = load_recovery_app_module()
        module.time.time = lambda: 1779450002
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.runtime = {"mode": "recovery"}
        app.config_store = FakeConfigStore(app.runtime)
        app.reboot_required = False
        app.reboot_deadline_ms = None
        app.logger = FakeLogger()

        result = app._handle_request(
            {
                "command": "reboot_to_os",
                "target_mode": "recovery",
                "expires_at_ms": 1779450001000,
            },
            None,
        )

        self.assertEqual(result["status"], "error")
        self.assertEqual(result["message"], "command_expired")
        self.assertFalse(result["reboot_required"])
        self.assertFalse(app.reboot_required)

    def test_recovery_reboot_to_recovery_is_wrong_mode(self):
        module = load_recovery_app_module()
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.runtime = {"mode": "recovery"}
        app.config_store = FakeConfigStore(app.runtime)
        app.reboot_required = False
        app.reboot_deadline_ms = None
        app.logger = FakeLogger()

        result = app._handle_request(
            {"command": "reboot_to_recovery", "target_mode": "normal"},
            None,
        )

        self.assertEqual(result["status"], "error")
        self.assertEqual(result["message"], "wrong_mode")
        self.assertFalse(result["reboot_required"])
        self.assertFalse(app.reboot_required)

    def test_recovery_reboot_to_os_requires_recovery_target_mode(self):
        module = load_recovery_app_module()
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.runtime = {"mode": "recovery"}
        app.config_store = FakeConfigStore(app.runtime)
        app.reboot_required = False
        app.reboot_deadline_ms = None
        app.logger = FakeLogger()

        result = app._handle_request(
            {"command": "reboot_to_os", "target_mode": "normal"},
            None,
        )

        self.assertEqual(result["status"], "error")
        self.assertEqual(result["message"], "wrong_mode")
        self.assertFalse(result["reboot_required"])
        self.assertFalse(app.reboot_required)

    def test_write_os_uses_request_release_url_in_recovery(self):
        module = load_recovery_app_module()
        writer_calls = []

        class FakeOSWriter:
            def __init__(self, *args, **kwargs):
                pass

            def write_os(self, release_url):
                writer_calls.append(release_url)
                return {
                    "status": "ok",
                    "message": "os_write_complete",
                    "downloaded_files": 1,
                    "skipped_files": 2,
                    "reboot_required": True,
                }

        module.OSWriter = FakeOSWriter
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

        result = app._handle_request(
            {"command": "write_os", "release_url": "http://192.168.1.2:8000/latest.json"},
            None,
        )

        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["message"], "os_write_complete")
        self.assertEqual(
            result["release_url"],
            "http://192.168.1.2:8000/latest.json",
        )
        self.assertEqual(writer_calls, ["http://192.168.1.2:8000/latest.json"])

    def test_write_os_uses_runtime_release_url_when_request_omits_it(self):
        module = load_recovery_app_module()
        writer_calls = []

        class FakeOSWriter:
            def __init__(self, *args, **kwargs):
                pass

            def write_os(self, release_url):
                writer_calls.append(release_url)
                return {
                    "status": "ok",
                    "message": "os_write_complete",
                    "downloaded_files": 1,
                    "skipped_files": 0,
                    "reboot_required": True,
                }

        module.OSWriter = FakeOSWriter
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.runtime = {
            "mode": "recovery",
            "update": {
                "release_url": "https://example.com/runtime-latest.json",
                "enabled": True,
            },
        }
        app.config_store = FakeConfigStore(app.runtime)
        app.os_writer = None
        app.logger = None

        result = app._handle_request({"command": "write_os"}, None)

        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["release_url"], "https://example.com/runtime-latest.json")
        self.assertEqual(writer_calls, ["https://example.com/runtime-latest.json"])

    def test_write_os_falls_back_to_default_release_url(self):
        module = load_recovery_app_module()
        writer_calls = []

        class FakeOSWriter:
            def __init__(self, *args, **kwargs):
                pass

            def write_os(self, release_url):
                writer_calls.append(release_url)
                return {
                    "status": "ok",
                    "message": "os_write_complete",
                    "downloaded_files": 1,
                    "skipped_files": 0,
                    "reboot_required": True,
                }

        module.OSWriter = FakeOSWriter
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.runtime = {"mode": "recovery", "update": {"enabled": True}}
        app.config_store = FakeConfigStore(app.runtime)
        app.os_writer = None
        app.logger = None

        result = app._handle_request({"command": "write_os"}, None)

        self.assertEqual(result["status"], "ok")
        self.assertEqual(
            result["release_url"],
            "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.json",
        )
        self.assertEqual(
            writer_calls,
            ["https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.json"],
        )

    def test_wifi_portal_post_reload_reconfigures_discovered_gateway_runtime(self):
        module = load_recovery_app_module()
        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.runtime = {
            "transport": {"mode": "udp_tcp"},
            "server": {"host": "", "tcp_port": 22345, "udp_port": 13250, "source": "findme", "gateway_id": ""},
            "findme": {"host": "", "gateway_id": "", "last_error": "findme_no_gateway"},
            "logging": {"enabled": True, "capacity": "default"},
        }
        app.config_store = FakeConfigStore({
            "transport": {"mode": "udp_tcp"},
            "server": {"host": "192.168.1.200", "tcp_port": 22345, "udp_port": 13250, "source": "findme", "gateway_id": "gw"},
            "findme": {"host": "192.168.1.200", "gateway_id": "gw", "last_error": ""},
            "logging": {"enabled": True, "capacity": "default"},
        })
        app.logger = FakeLogger()
        app.control_transport = FakeTCPControlTransport()
        app.wifi = FakePortalWiFi(handled=True)

        handled = app._service_wifi_setup_portal()

        self.assertTrue(handled)
        self.assertEqual(app.runtime["server"]["host"], "192.168.1.200")
        self.assertEqual(app.runtime["server"]["tcp_port"], 22345)
        self.assertEqual(app.runtime["server"]["udp_port"], 13250)
        self.assertEqual(app.runtime["server"]["gateway_id"], "gw")
        self.assertEqual(app.runtime["findme"]["last_error"], "")
        self.assertEqual(app.control_transport.reconfigure_calls, 1)
        self.assertIn("runtime_config_reloaded source=wifi_portal", app.logger.infos)

    def test_recovery_direct_sta_link_no_ip_falls_back_to_setup_portal(self):
        module = load_recovery_app_module()

        class LinkNoIpWiFi:
            def __init__(self):
                self.state = "idle"
                self.fallback_calls = []
                self.findme_results = []

            def connect(self):
                self.state = "wifi_link_no_ip"
                return False

            def fallback_to_setup_portal(self, reason):
                self.fallback_calls.append(reason)
                self.state = "wifi_setup_active"
                return True

            def start_setup_portal(self, reason):
                self.fallback_calls.append(reason)
                self.state = "wifi_setup_active"
                return True

            def is_connected(self):
                return False

        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.device_name = "New Horizons OS"
        app.device_uid = "UID123"
        app.wifi_setup_requested = False
        app.config_store = FakeConfigStore({"mode": "recovery"})
        app.runtime = {"mode": "recovery"}
        app.logger = FakeLogger()
        app.wifi = LinkNoIpWiFi()
        app.boot_network_initialized = False
        app._os_installed = lambda: True
        app._run_findme = lambda reason="boot": app.wifi.findme_results.append(reason)
        app._announce_status = lambda *args, **kwargs: None

        app.setup()

        self.assertEqual(app.wifi.fallback_calls, ["dhcp_no_ip"])
        self.assertFalse(app.boot_network_initialized)
        self.assertEqual(app.wifi.findme_results, [])

    def test_recovery_boot_request_wifi_setup_starts_portal_without_sta_connect(self):
        module = load_recovery_app_module()

        class PortalWiFi:
            def __init__(self):
                self.setup_started = []
                self.connect_calls = 0

            def start_setup_portal(self, reason):
                self.setup_started.append(reason)
                return True

            def connect(self):
                self.connect_calls += 1
                return True

            def is_connected(self):
                return False

        app = module.RecoveryApp.__new__(module.RecoveryApp)
        app.device_name = "New Horizons OS"
        app.device_uid = "UID123"
        app.wifi_setup_requested = False
        app.config_store = FakeConfigStore({"mode": "recovery", "boot_request": "wifi_setup"})
        app.runtime = {"mode": "recovery", "boot_request": "wifi_setup"}
        app.logger = FakeLogger()
        app.wifi = PortalWiFi()
        app.boot_network_initialized = False
        app._os_installed = lambda: True
        app._run_findme = lambda reason="boot": None
        app._announce_status = lambda *args, **kwargs: None

        app.setup()

        self.assertEqual(app.wifi.setup_started, ["boot_request"])
        self.assertEqual(app.wifi.connect_calls, 0)
        self.assertFalse(app.boot_network_initialized)


if __name__ == "__main__":
    unittest.main()
