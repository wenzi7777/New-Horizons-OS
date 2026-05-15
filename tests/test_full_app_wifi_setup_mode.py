import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FULL_APP_PATH = REPO_ROOT / "device" / "channels" / "full" / "files" / "app.py"


class FakeScan:
    def __init__(self, events=None):
        self.init_calls = []
        self.start_calls = 0
        self.events = events if events is not None else []

    def init(self, **kwargs):
        self.init_calls.append(kwargs)

    def start(self):
        self.events.append("scan_start")
        self.start_calls += 1

    def service(self):
        return False

    def pop_frame_mv(self):
        return None

    def stats(self):
        return ()


class FakeLogger:
    def info(self, _message):
        pass

    def warn(self, _message):
        pass

    def error(self, _message):
        pass


class FakeWiFi:
    def __init__(self, events=None):
        self.setup_started = []
        self.connected = False
        self.portal_active = False
        self.events = events if events is not None else []

    def start_setup_portal(self, reason):
        self.setup_started.append(reason)
        self.portal_active = True
        return True

    def setup_active(self):
        return self.portal_active

    def connect(self):
        self.events.append("wifi_connect")
        self.connected = True
        return True

    def is_connected(self):
        return self.connected

    def service_setup_portal(self):
        return None

    def portal_status(self):
        return {"active": self.portal_active}


class FakeRuntimeConfigStore:
    def __init__(self, runtime_override=None):
        self.runtime = {
            "scan_timing": {"target_fps": 60, "settle_us": 20, "core_id": 1},
            "buffer_frames": 8,
            "master_server": {},
            "matrix_layout": {"active_rows": [1], "active_cols": [1]},
        }
        if runtime_override:
            self.runtime.update(runtime_override)

    def load_runtime(self):
        return dict(self.runtime)

    def update_runtime(self, patch):
        for key, value in patch.items():
            if isinstance(value, dict) and isinstance(self.runtime.get(key), dict):
                merged = dict(self.runtime[key])
                merged.update(value)
                self.runtime[key] = merged
            else:
                self.runtime[key] = value
        return dict(self.runtime)

    def load_network(self):
        return {"ssid": "saved"}

    def load_filter(self):
        return {"enabled": False, "median": 3, "alpha": 0.25}


def load_full_app_module(runtime_override=None):
    events = []
    fake_scan = FakeScan(events)
    fake_vdboard = types.SimpleNamespace(scan=fake_scan)
    saved_modules = {}
    injected = {
        "machine": types.SimpleNamespace(reset=lambda: None),
        "config": types.SimpleNamespace(
            DEVICE_NAME="New Horizons OS",
            ENABLE_IMU=False,
            ENABLE_BATTERY=False,
            ENABLE_LED=False,
            USE_PACKET_BUFFER=False,
            DEVICE_STATE_DIR="device_state",
            CALIBRATION_DIR="device_state/calibration",
            LOG_PATH="device_state/logs/device.log",
            UDP_CONTROL_PORT=22345,
            STATUS_ANNOUNCE_INTERVAL_MS=2000,
            TARGET_FPS=60,
            MATRIX_SETTLE_US=20,
            IMU_RATE_HZ=60,
            BATTERY_RATE_HZ=2,
            LED_RATE_HZ=20,
            GC_EVERY_N_FRAMES=0,
            ACTIVE_ROWS=[1],
            ACTIVE_COLS=[1],
            AVAILABLE_ROWS=[1],
            AVAILABLE_COLS=[1],
            ROWS=1,
            COLS=1,
            SEND_EVERY_N_FRAMES=1,
            PACKET_BUFFER_SIZE=1,
            PACKET_BUFFER_DROP_OLDEST=False,
            PRINT_PIN_CONFLICTS=False,
            PRINT_FPS=False,
            UDP_SERVER_IP="127.0.0.1",
            UDP_SERVER_PORT=5005,
        ),
        "board_pins": types.SimpleNamespace(validate_pins=lambda: {}),
        "vdboard": fake_vdboard,
        "calibration_store": types.SimpleNamespace(
            CalibrationStore=lambda root: types.SimpleNamespace(load=lambda: None, apply=lambda *args: 0.0, list_levels=lambda: [])
        ),
        "device_identity": types.SimpleNamespace(
            get_device_id=lambda: 0x12345678,
            get_device_uid=lambda: "UID123",
            get_device_name=lambda default: default,
        ),
        "device_logging": types.SimpleNamespace(DeviceLogger=lambda path: FakeLogger()),
        "filesystem_api": types.SimpleNamespace(FilesystemAPI=lambda root: None),
        "filter_engine": types.SimpleNamespace(FilterChain=lambda **kwargs: types.SimpleNamespace(process=lambda idx, value: value, apply_config=lambda *args: None)),
        "frame_protocol": types.SimpleNamespace(decode_scan_frame=lambda payload: {}),
        "packet": types.SimpleNamespace(PacketBuilder=lambda **kwargs: None),
        "packet_buffer": types.SimpleNamespace(PacketBuffer=lambda **kwargs: None),
        "runtime_config": types.SimpleNamespace(
            RuntimeConfigStore=lambda root: FakeRuntimeConfigStore(runtime_override)
        ),
        "time_sync": types.SimpleNamespace(
            TimeSync=lambda servers: types.SimpleNamespace(
                status=lambda: {"synced": False},
                sync=lambda: True,
                now_epoch=lambda: 0,
                last_error="",
            )
        ),
        "udp_control": types.SimpleNamespace(UDPControlServer=lambda port, logger=None: types.SimpleNamespace(begin=lambda: None, poll=lambda handler: None, sock=object(), send=lambda host, port, payload: True)),
        "udp_stream": types.SimpleNamespace(UDPStreamer=lambda host, port: types.SimpleNamespace(send=lambda payload: True)),
        "update_manager": types.SimpleNamespace(
            UpdateManager=lambda *args, **kwargs: types.SimpleNamespace(
                service=lambda: None,
                status=lambda: {},
                check=lambda: {"message": "update_disabled"},
            )
        ),
        "utils": types.SimpleNamespace(RateCounter=lambda interval: None),
        "wifi_manager": types.SimpleNamespace(WiFiManager=lambda *args, **kwargs: FakeWiFi(events)),
    }
    for name, module in injected.items():
        saved_modules[name] = sys.modules.get(name)
        sys.modules[name] = module

    try:
        spec = importlib.util.spec_from_file_location("full_app_wifi_setup_test", FULL_APP_PATH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.time.ticks_ms = lambda: 0
        module.time.ticks_diff = lambda now, then: now - then
        return module, fake_scan, events, saved_modules
    except Exception:
        for name, saved in saved_modules.items():
            if saved is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = saved
        raise


class FullAppWifiSetupModeTests(unittest.TestCase):
    def test_wifi_setup_mode_starts_portal_before_scan_hardware_init(self):
        module, fake_scan, _events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=True)
            app.setup()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(app.wifi.setup_started, ["boot_window"])
        self.assertEqual(fake_scan.init_calls, [])
        self.assertEqual(fake_scan.start_calls, 0)

    def test_normal_boot_connects_wifi_before_starting_scan_hardware(self):
        module, _fake_scan, events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(events[:2], ["wifi_connect", "scan_start"])

    def test_empty_matrix_layout_skips_scan_start(self):
        module, fake_scan, events, saved_modules = load_full_app_module(
            runtime_override={"matrix_layout": {"active_rows": [], "active_cols": []}}
        )
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(events, ["wifi_connect"])
        self.assertEqual(fake_scan.init_calls, [])
        self.assertEqual(fake_scan.start_calls, 0)
        self.assertFalse(app.scan_ready)

    def test_set_matrix_layout_persists_valid_pin_selection(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)
            response = app._handle_control_request(
                {"command": "set_matrix_layout", "active_rows": [1], "active_cols": [1]},
                ("127.0.0.1", 22345),
            )
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertEqual(response["runtime"]["matrix_layout"], {"active_rows": [1], "active_cols": [1]})


if __name__ == "__main__":
    unittest.main()
