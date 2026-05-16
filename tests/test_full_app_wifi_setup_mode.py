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
    def __init__(self):
        self.infos = []
        self.warns = []
        self.errors = []

    def info(self, message):
        self.infos.append(message)

    def warn(self, message):
        self.warns.append(message)

    def error(self, message):
        self.errors.append(message)


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


class FakeLED:
    def __init__(self):
        self.states = []

    def begin(self):
        pass

    def set_boot_window(self):
        self.states.append("boot_window")

    def set_wifi_setup(self):
        self.states.append("wifi_setup")

    def set_updating(self):
        self.states.append("updating")

    def set_normal(self):
        self.states.append("normal")

    def set_error(self):
        self.states.append("error")


class FakeControl:
    def __init__(self):
        self.sock = None
        self.begin_calls = 0

    def begin(self):
        self.begin_calls += 1
        self.sock = object()

    def poll(self, handler):
        return None

    def send(self, host, port, payload):
        return True


def load_full_app_module(runtime_override=None, update_check=None, enable_led=False):
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
            ENABLE_LED=enable_led,
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
        "packet": types.SimpleNamespace(
            PacketBuilder=lambda **kwargs: types.SimpleNamespace(build=lambda **packet_kwargs: b"packet")
        ),
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
        "udp_control": types.SimpleNamespace(UDPControlServer=lambda port, logger=None: FakeControl()),
        "udp_stream": types.SimpleNamespace(UDPStreamer=lambda host, port: types.SimpleNamespace(send=lambda payload: True)),
        "update_manager": types.SimpleNamespace(
            UpdateManager=lambda *args, **kwargs: types.SimpleNamespace(
                service=lambda: None,
                status=lambda: {},
                check=update_check or (lambda: {"message": "update_disabled"}),
            )
        ),
        "utils": types.SimpleNamespace(RateCounter=lambda interval: None),
        "wifi_manager": types.SimpleNamespace(WiFiManager=lambda *args, **kwargs: FakeWiFi(events)),
    }
    if enable_led:
        injected["sk6812"] = types.SimpleNamespace(SK6812Status=FakeLED)
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

    def test_runtime_services_are_deferred_until_after_wifi_boot(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)

            self.assertIsNone(app.update_manager)
            self.assertIsNone(app.control)
            self.assertIsNone(app.time_sync)
            self.assertIsNone(app.calibration)
            self.assertIsNone(app.tx_buffer)
            self.assertIsNone(app.packet)
            self.assertIsNone(app.filter_chain)

            app.setup()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertIsNotNone(app.update_manager)
        self.assertIsNotNone(app.control)
        self.assertIsNotNone(app.time_sync)
        self.assertIsNotNone(app.calibration)
        self.assertIsNotNone(app.packet)
        self.assertIsNotNone(app.filter_chain)

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

    def test_gc_collects_once_per_frame_interval(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        calls = []
        try:
            app = module.App.__new__(module.App)
            app.frame_id = 120
            app.last_gc_frame_id = 0
            module.config.GC_EVERY_N_FRAMES = 120
            module.gc.collect = lambda: calls.append(app.frame_id)

            self.assertTrue(app._maybe_collect_garbage())
            self.assertFalse(app._maybe_collect_garbage())

            app.frame_id = 241
            self.assertTrue(app._maybe_collect_garbage())
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(calls, [120, 241])

    def test_failed_transmit_enters_backoff_and_drops_stale_packets(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()

        class FakeBuffer:
            def __init__(self):
                self.items = [b"old"]
                self.clear_calls = 0

            def clear(self):
                self.clear_calls += 1
                self.items = []

            def pop(self):
                return self.items.pop(0) if self.items else None

        try:
            app = module.App.__new__(module.App)
            app.udp = types.SimpleNamespace(send=lambda payload: False)
            app.wifi = types.SimpleNamespace(is_connected=lambda: True)
            app.tx_buffer = FakeBuffer()
            app.sent_packets = 0
            app.failed_sends = 0
            app.send_backoff_until_ms = 0
            module.config.USE_PACKET_BUFFER = True
            module.config.SEND_MAX_PER_LOOP = 1
            module.config.SEND_FAILURE_BACKOFF_MS = 100
            module.time.ticks_ms = lambda: 1000
            module.time.ticks_add = lambda now, delta: now + delta
            module.time.ticks_diff = lambda now, then: now - then

            app.handle_transmit()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(app.failed_sends, 1)
        self.assertEqual(app.send_backoff_until_ms, 1100)
        self.assertEqual(app.tx_buffer.clear_calls, 1)

    def test_scan_skips_packet_build_while_transmit_is_backing_off(self):
        module, fake_scan, _events, saved_modules = load_full_app_module()
        build_calls = []
        try:
            fake_scan.pop_frame_mv = lambda: memoryview(b"frame")
            module.decode_scan_frame = lambda payload: {
                "payload_mv": [1],
                "seq": 7,
                "timestamp_ms": 700,
            }

            app = module.App.__new__(module.App)
            app.runtime = {"scan_timing": {"send_every_n_frames": 1}}
            app.latest_frame = None
            app.latest_matrix = None
            app.latest_imu = None
            app.latest_battery = None
            app.vdboard = types.SimpleNamespace(scan=fake_scan)
            app.decode_scan_frame = module.decode_scan_frame
            app.frame_id = 0
            app.send_backoff_until_ms = 1100
            app.time_sync = types.SimpleNamespace(status=lambda: {"synced": False})
            app.scan_rate = types.SimpleNamespace(tick=lambda: None)
            app.udp = types.SimpleNamespace(send=lambda payload: True)
            app.wifi = types.SimpleNamespace(is_connected=lambda: True)
            app.sent_packets = 0
            app.failed_sends = 0
            app.packet = types.SimpleNamespace(
                build=lambda **kwargs: build_calls.append(kwargs) or b"packet"
            )
            app._apply_sensor_pipeline = lambda matrix: [float(matrix[0])]
            module.config.SEND_EVERY_N_FRAMES = 1
            module.config.PRINT_FPS = False
            module.time.ticks_diff = lambda now, then: now - then

            app.handle_scan(1000)
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(app.frame_id, 7)
        self.assertEqual(app.latest_matrix, [1.0])
        self.assertEqual(build_calls, [])

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

    def test_boot_update_check_failure_does_not_escape_or_leave_led_updating(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module(
            runtime_override={"update": {"check_on_boot": True}},
            update_check=lambda: (_ for _ in ()).throw(MemoryError("manifest parse oom")),
            enable_led=True,
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

        self.assertIn("updating", app.led.states)
        self.assertEqual(app.led.states[-1], "normal")
        self.assertTrue(any("update_check_failed" in message for message in app.logger.warns))


if __name__ == "__main__":
    unittest.main()
