import importlib.util
import struct
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FULL_APP_PATH = REPO_ROOT / "device" / "os" / "app.py"


class FakeScan:
    def __init__(self, events=None):
        self.init_calls = []
        self.start_calls = 0
        self.stop_calls = 0
        self.events = events if events is not None else []

    def init(self, **kwargs):
        self.init_calls.append(kwargs)

    def start(self):
        self.events.append("scan_start")
        self.start_calls += 1

    def stop(self):
        self.events.append("scan_stop")
        self.stop_calls += 1

    def service(self):
        return False

    def pop_frame_mv(self):
        return None

    def stats(self):
        return ()


class FailingInitScan(FakeScan):
    def init(self, **kwargs):
        super().init(**kwargs)
        raise MemoryError("stream buffer alloc failed")


class NativePacketScan(FakeScan):
    def __init__(self, events=None):
        super().__init__(events)
        self.pop_packet_calls = 0
        self.packet = b"native-packet"

    def set_packet_options(self, *args):
        return True

    def load_calibration(self, *args):
        return True

    def configure_filter(self, *args):
        return True

    def update_imu_cache(self, *args):
        return True

    def update_battery_cache(self, *args):
        return True

    def pop_packet(self):
        self.pop_packet_calls += 1
        return self.packet

    def pop_packet_into(self, buffer):
        self.pop_packet_calls += 1
        if self.packet is None:
            return None
        buffer[:len(self.packet)] = self.packet
        return len(self.packet)

    def memory_stats(self):
        return {
            "heap_free": 65536,
            "heap_largest_free_block": 32768,
            "packet_scratch_bytes": 256,
        }

    def stream_stats(self):
        return {
            "produced_frames": 12,
            "consumed_frames": 11,
            "dropped_frames": 1,
            "packet_frames": self.pop_packet_calls,
            "ring_count": 0,
            "buffer_frames": 2,
            "point_count": 1,
            "imu_cached": True,
            "battery_cached": True,
        }

    def stats(self):
        return (1, 0, 0, 1, 0, 8, 1, 1, 1, 1)


class FrameSampleScan(NativePacketScan):
    def __init__(self, frame, events=None):
        super().__init__(events)
        self.frame = frame

    def service(self):
        return True

    def pop_frame_mv(self):
        return self.frame


class FakeLogger:
    def __init__(self):
        self.infos = []
        self.warns = []
        self.errors = []
        self.enabled = True
        self.capacity = "default"
        self.max_bytes = 16384

    def info(self, message):
        self.infos.append(message)

    def warn(self, message):
        self.warns.append(message)

    def error(self, message):
        self.errors.append(message)

    def configure(self, enabled=True, capacity="default"):
        self.enabled = bool(enabled)
        self.capacity = capacity if capacity in ("default", "extended") else "default"
        self.max_bytes = 65536 if self.capacity == "extended" else 16384

    def settings(self):
        return {
            "enabled": self.enabled,
            "capacity": self.capacity,
            "serial": "status",
            "max_bytes": self.max_bytes,
        }


class FakeWiFi:
    def __init__(self, events=None, connect_result=True):
        self.setup_started = []
        self.connected = False
        self.portal_active = False
        self.events = events if events is not None else []
        self.state = "idle"
        self.connect_result = connect_result
        self.findme_results = []

    def start_setup_portal(self, reason):
        self.setup_started.append(reason)
        self.portal_active = True
        self.state = "setup"
        return True

    def setup_active(self):
        return self.portal_active

    def connect(self):
        self.events.append("wifi_connect")
        self.connected = bool(self.connect_result)
        self.state = "connected" if self.connected else "offline"
        return self.connected

    def is_connected(self):
        return self.connected

    def service_setup_portal(self):
        return None

    def portal_status(self):
        return {"active": self.portal_active}

    def run_findme(self, reason="manual"):
        self.findme_results.append(reason)
        return {
            "ok": True,
            "host": "127.0.0.1",
            "tcp_port": 22345,
            "udp_port": 13250,
            "gateway_id": "test-gateway",
        }


class FakeRuntimeConfigStore:
    def __init__(self, runtime_override=None):
        self.runtime = {
            "scan_timing": {"target_fps": 60, "settle_us": 20, "core_id": 1},
            "buffer_frames": 8,
            "matrix_layout": {"active_rows": [1], "active_cols": [1]},
            "matrix_layout_state": {"pending": False, "committed": True, "last_error": ""},
            "matrix_scan_state": {"active": False, "autostart_disabled": False, "last_error": ""},
            "server": {"host": "127.0.0.1", "tcp_port": 22345, "udp_port": 13250, "source": "findme", "gateway_id": "test-gateway"},
            "findme": {"host": "127.0.0.1", "gateway_id": "test-gateway", "last_error": ""},
            "transport": {"mode": "udp_tcp"},
            "logging": {"enabled": True, "capacity": "default", "serial": "status"},
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
    def __init__(self, events=None):
        self.events = events if events is not None else []
        self.states = []

    def begin(self):
        self.events.append("led_begin")
        pass

    def set_boot_window(self):
        self.events.append("led_boot_window")
        self.states.append("boot_window")

    def set_wifi_setup(self):
        self.states.append("wifi_setup")

    def set_updating(self):
        self.states.append("updating")

    def set_reboot_required(self):
        self.states.append("reboot_required")

    def set_maintenance(self):
        self.states.append("maintenance")

    def set_normal(self):
        self.states.append("normal")

    def set_charging(self):
        self.states.append("charging")

    def set_charge_done(self):
        self.states.append("charge_done")

    def set_error(self):
        self.states.append("error")

    def set_findme_no_gateway(self):
        self.states.append("findme_no_gateway")

    def set_findme_gateway_lost(self):
        self.states.append("findme_gateway_lost")

    def set_findme_rejected(self):
        self.states.append("findme_rejected")


class FakeBattery:
    STATUS_UNKNOWN = 0
    STATUS_NOT_CHARGING = 1
    STATUS_CHARGING_CC = 2
    STATUS_DONE = 4

    def __init__(self):
        self.begin_calls = 0
        self.last_status_code = self.STATUS_UNKNOWN
        self.next_status_code = self.STATUS_NOT_CHARGING
        self.read_calls = 0

    def begin(self):
        self.begin_calls += 1
        return True

    def read_status(self):
        self.read_calls += 1
        self.last_status_code = self.next_status_code
        return (self.last_status_code, 0, 0)

    def is_charging(self):
        return self.last_status_code == self.STATUS_CHARGING_CC

    def is_charge_done(self):
        return self.last_status_code == self.STATUS_DONE


class FakeIMU:
    def __init__(self):
        self.begin_calls = 0
        self.read_calls = 0

    def begin(self):
        self.begin_calls += 1
        return True

    def read(self):
        self.read_calls += 1
        return (0, 0, 0, 0, 0, 0, 0)


def load_full_app_module(runtime_override=None, update_check=None, enable_led=False, enable_battery=False, enable_imu=False, wifi_connect_result=True):
    events = []
    fake_scan = NativePacketScan(events)
    fake_vdboard = types.SimpleNamespace(scan=fake_scan)
    saved_modules = {}

    class ForbiddenMqttModule(types.ModuleType):
        def __getattr__(self, name):
            raise AssertionError("New Horizons OS must not import MQTT modules")

    class FakeTCPControlTransport:
        def __init__(self, *args, **kwargs):
            self.poll_calls = 0
            self.status_payloads = []
            self.progress_payloads = []
            self.flush_calls = []
            self.reconfigure_calls = 0

        def poll(self, *args, **kwargs):
            self.poll_calls += 1

        def publish_status(self, payload, connected):
            self.status_payloads.append((payload, connected))
            return True

        def publish_update_progress(self, payload, connected):
            self.progress_payloads.append((payload, connected))
            return True

        def flush(self, max_bytes=None):
            self.flush_calls.append(max_bytes)
            return True

        def reconfigure(self):
            self.reconfigure_calls += 1

        def is_connected(self):
            return True

    class FakeUDPStreamTransport:
        def __init__(self, *args, **kwargs):
            self.sent = []
            self.reconfigure_calls = 0

        def send(self, payload, connected):
            self.sent.append((payload, connected))
            return True

        def reconfigure(self):
            self.reconfigure_calls += 1

    def fake_calibration_store(root):
        def set_point(*args):
            events.append(("calibration_set_point", args))

        events.append("calibration_store_load")
        return types.SimpleNamespace(
            load=lambda: None,
            apply=lambda *args: 0.0,
            list_levels=lambda: [],
            set_point=set_point,
            save=lambda: None,
            dump=lambda: {},
            dump_level=lambda *args: {},
            delete_level=lambda *args: None,
        )

    def fake_filesystem_api(*args, **kwargs):
        events.append("filesystem_api_load")
        return types.SimpleNamespace(
            list_files=lambda scope="user": [],
            usage=lambda: {},
            upload_begin=lambda *args: {"ok": True},
            upload_chunk=lambda *args: {"ok": True},
            upload_finish=lambda *args: {"ok": True},
            download_begin=lambda *args: {"ok": True},
            download_chunk=lambda *args: {"ok": True},
            delete_file=lambda *args: True,
            list_tree=lambda *args: [],
            read_file=lambda *args: None,
        )

    injected = {
        "machine": types.SimpleNamespace(reset=lambda: None),
        "immutable_config": types.SimpleNamespace(
            FIRMWARE_VERSION="v-recovery-test",
            RECOVERY_VERSION="v-recovery-test",
        ),
        "config": types.SimpleNamespace(
            DEVICE_NAME="New Horizons OS",
            HARDWARE_MODEL="VD-CTL/R v1.0.F 2026.4",
            RUNTIME_VERSION="v-runtime-test",
            FIRMWARE_VERSION="v-os-test",
            ENABLE_IMU=enable_imu,
            ENABLE_BATTERY=enable_battery,
            ENABLE_LED=enable_led,
            KEEP_CALIBRATION_MODULE_LOADED=True,
            USE_PACKET_BUFFER=False,
            DEVICE_STATE_DIR="device_state",
            CALIBRATION_DIR="device_state/calibration",
            LOG_PATH="device_state/logs/device.log",
            STATUS_ANNOUNCE_INTERVAL_MS=2000,
            TARGET_FPS=60,
            MAX_FPS=90,
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
            DATA_FILES_DIR="data/files",
            DATA_LOG_DIR="data/logs",
            DATA_TMP_DIR="data/tmp",
        ),
        "board_pins": types.SimpleNamespace(validate_pins=lambda: {}),
        "vdboard": fake_vdboard,
        "calibration_store": types.SimpleNamespace(CalibrationStore=fake_calibration_store),
        "device_identity": types.SimpleNamespace(
            get_device_id=lambda: "AABBCCDDEEFF",
            get_device_uid=lambda: "UID123",
            get_packet_device_uid_bytes=lambda: b"\xaa\xbb\xcc\xdd\xee\xff",
            get_device_name=lambda default: default,
        ),
        "device_logging": types.SimpleNamespace(DeviceLogger=lambda *args, **kwargs: FakeLogger()),
        "filesystem_api": types.SimpleNamespace(FilesystemAPI=fake_filesystem_api),
        "filter_engine": types.SimpleNamespace(FilterChain=lambda **kwargs: types.SimpleNamespace(process=lambda idx, value: value, apply_config=lambda *args: None)),
        "frame_protocol": types.SimpleNamespace(decode_scan_frame=lambda payload: {}),
        "mqtt_transport": ForbiddenMqttModule("mqtt_transport"),
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
        "tcp_control": types.SimpleNamespace(TCPControlTransport=FakeTCPControlTransport),
        "udp_control": types.ModuleType("udp_control"),
        "udp_stream": types.SimpleNamespace(UDPStreamTransport=FakeUDPStreamTransport),
        "utils": types.SimpleNamespace(RateCounter=lambda interval: None),
        "wifi_manager": types.SimpleNamespace(
            WiFiManager=lambda *args, **kwargs: FakeWiFi(events, connect_result=wifi_connect_result)
        ),
    }
    if enable_led:
        injected["sk6812"] = types.SimpleNamespace(SK6812Status=lambda: FakeLED(events))
    if enable_battery:
        injected["bq25180"] = types.SimpleNamespace(BQ25180=FakeBattery)
    if enable_imu:
        injected["bmi270"] = types.SimpleNamespace(BMI270=FakeIMU)
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
        module, fake_scan, events, saved_modules = load_full_app_module()
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

        boot_order = [event for event in events if event in ("wifi_connect", "scan_start")]
        self.assertEqual(boot_order[:2], ["wifi_connect", "scan_start"])

    def test_normal_boot_connects_wifi_before_led_initialization(self):
        module, _fake_scan, events, saved_modules = load_full_app_module(enable_led=True)
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertLess(events.index("wifi_connect"), events.index("led_begin"))

    def test_reboot_required_uses_dedicated_no_power_off_led_state(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module(enable_led=True)
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
            app.reboot_required = True
            app.update_led_state()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(app.led.states[-1], "reboot_required")

    def test_maintenance_mode_uses_dedicated_led_state(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module(enable_led=True)
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
            app.mode = "maintenance"
            app.update_led_state()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(app.led.states[-1], "maintenance")

    def test_battery_status_refresh_updates_led_when_usb_power_starts_charging(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module(enable_led=True, enable_battery=True)
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
            self.assertEqual(app.led.states[-1], "normal")

            app.battery.next_status_code = app.battery.STATUS_CHARGING_CC
            changed = app._service_battery_status()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertTrue(changed)
        self.assertEqual(app.led.states[-1], "charging")
        self.assertEqual(app._status()["battery"]["state"], "charging")
        self.assertEqual(app._status_announce_payload()["battery"]["state"], "charging")

    def test_status_reports_charge_done_state(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module(enable_led=True, enable_battery=True)
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()

            app.battery.next_status_code = app.battery.STATUS_DONE
            app._service_battery_status()
            status = app._status()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(status["battery"]["state"], "charge_done")
        self.assertTrue(status["battery"]["charge_done"])

    def test_reboot_required_led_overrides_battery_charging(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module(enable_led=True, enable_battery=True)
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()

            app.reboot_required = True
            app.battery.next_status_code = app.battery.STATUS_CHARGING_CC
            changed = app._service_battery_status()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertTrue(changed)
        self.assertEqual(app.led.states[-1], "reboot_required")

    def test_status_includes_system_versions(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        try:
            module.gc.mem_free = lambda: 123456
            module.gc.mem_alloc = lambda: 65432
            app = module.App(wifi_setup_requested=False)
            status = app._status()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(status["system"]["name"], "New Horizons OS")
        self.assertEqual(status["system"]["mode"], "normal")
        self.assertEqual(status["system"]["hardware_model"], "VD-CTL/R v1.0.F 2026.4")
        self.assertEqual(status["system"]["runtime_version"], "v-runtime-test")
        self.assertEqual(status["system"]["os_version"], "v-os-test")
        self.assertEqual(status["system"]["recovery_version"], "v-recovery-test")
        self.assertEqual(status["memory"]["heap_free"], 123456)
        self.assertEqual(status["memory"]["heap_allocated"], 65432)
        self.assertEqual(status["memory"]["heap_total"], 188888)

    def test_status_reports_requested_and_measured_scan_fps(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module(
            runtime_override={"scan_timing": {"target_fps": 75, "settle_us": 20, "core_id": 1}}
        )
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
            app.scan_rate = types.SimpleNamespace(rate=41.5)
            app.sent_packets = 20
            app.failed_sends = 2

            status = app._status()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        health = status["scan_health"]
        self.assertEqual(health["requested_target_fps"], 75)
        self.assertEqual(health["measured_scan_fps"], 41.5)
        self.assertNotIn("effective_target_fps", health)
        self.assertEqual(health["sent_packets"], 20)
        self.assertEqual(health["failed_sends"], 2)
        self.assertEqual(health["stream"]["produced_frames"], 12)

    def test_scan_health_command_is_lightweight_status(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module(
            runtime_override={"scan_timing": {"target_fps": 75, "settle_us": 20, "core_id": 1}}
        )
        try:
            module.gc.mem_free = lambda: 30000
            module.gc.mem_alloc = lambda: 198000
            app = module.App(wifi_setup_requested=False)
            app.setup()
            app.scan_rate = types.SimpleNamespace(rate=42)
            app.sent_packets = 30
            app.failed_sends = 1

            response = app._handle_control_request({"command": "scan_health"}, ("tcp", 0))
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertEqual(response["command"], "scan_health")
        self.assertEqual(response["message"], "scan_health")
        self.assertEqual(response["requested_target_fps"], 75)
        self.assertEqual(response["measured_scan_fps"], 42)
        self.assertEqual(response["sent_packets"], 30)
        self.assertEqual(response["failed_sends"], 1)
        self.assertEqual(response["memory"]["heap_free"], 30000)
        self.assertEqual(response["native"]["heap_largest_free_block"], 32768)
        self.assertEqual(response["stream"]["point_count"], 1)

    def test_runtime_services_are_deferred_until_after_wifi_boot(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)

            self.assertIsNone(app.control_transport)
            self.assertIsNone(app.udp_stream)
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

        self.assertIsNotNone(app.control_transport)
        self.assertIsNotNone(app.udp_stream)
        self.assertIsNotNone(app.time_sync)
        self.assertIsNone(app.calibration)
        self.assertIsNone(app.packet)
        self.assertIsNone(app.filter_chain)

    def test_interrupted_scan_session_disables_boot_autostart(self):
        module, fake_scan, events, saved_modules = load_full_app_module(
            runtime_override={
                "matrix_scan_state": {"active": True, "autostart_disabled": False},
            }
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
        self.assertEqual(fake_scan.start_calls, 0)
        self.assertFalse(app.scan_ready)
        self.assertTrue(app.last_matrix_start_failed)
        self.assertTrue(app.config_store.runtime["matrix_scan_state"]["autostart_disabled"])

    def test_uncommitted_legacy_matrix_layout_does_not_autostart_scan(self):
        module, fake_scan, events, saved_modules = load_full_app_module(
            runtime_override={
                "matrix_layout": {"active_rows": [1], "active_cols": [1]},
                "matrix_layout_state": {"pending": False, "committed": False, "last_error": ""},
                "matrix_scan_state": {"active": False, "autostart_disabled": False, "last_error": ""},
            }
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
        self.assertEqual(fake_scan.start_calls, 0)
        self.assertFalse(app.scan_ready)
        self.assertTrue(app.last_matrix_start_failed)
        self.assertTrue(app.config_store.runtime["matrix_scan_state"]["autostart_disabled"])
        self.assertEqual(app.config_store.runtime["matrix_layout_state"]["last_error"], "layout_requires_reapply")

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

    def test_normal_boot_does_not_start_ap_portal_after_sta_connect_failure(self):
        module, fake_scan, events, saved_modules = load_full_app_module(wifi_connect_result=False)
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
        self.assertEqual(app.wifi.setup_started, [])
        self.assertFalse(app.boot_network_initialized)
        self.assertEqual(fake_scan.init_calls, [])
        self.assertEqual(fake_scan.start_calls, 0)

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
            app.udp_stream = types.SimpleNamespace(send=lambda payload, connected: False)
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

    def test_non_native_scan_is_stopped_instead_of_using_python_packet_builder(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        build_calls = []
        fake_scan = FakeScan([])
        try:
            app = module.App.__new__(module.App)
            app.runtime = {"scan_timing": {"send_every_n_frames": 1}}
            app.latest_frame = None
            app.latest_matrix = None
            app.latest_imu = None
            app.latest_battery = None
            app.vdboard = types.SimpleNamespace(scan=fake_scan)
            app.scan_ready = True
            app.tx_buffer = None
            app.logger = FakeLogger()
            app.config_store = types.SimpleNamespace(update_runtime=lambda patch: app.runtime)
            app.decode_scan_frame = lambda payload: (_ for _ in ()).throw(AssertionError("decode_scan_frame must not run"))
            app.frame_id = 0
            app.send_backoff_until_ms = 1100
            app.time_sync = types.SimpleNamespace(status=lambda: {"synced": False})
            app.scan_rate = types.SimpleNamespace(tick=lambda: None)
            app.udp_stream = types.SimpleNamespace(send=lambda payload, connected: True)
            app.wifi = types.SimpleNamespace(is_connected=lambda: True)
            app.sent_packets = 0
            app.failed_sends = 0
            app.packet = types.SimpleNamespace(
                build=lambda **kwargs: build_calls.append(kwargs) or b"packet"
            )
            app._apply_sensor_pipeline = lambda matrix: (_ for _ in ()).throw(AssertionError("pipeline must not run"))
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

        self.assertEqual(fake_scan.stop_calls, 1)
        self.assertTrue(app.last_matrix_start_failed)
        self.assertEqual(build_calls, [])

    def test_native_scan_skips_packet_pop_while_transmit_is_backing_off(self):
        module, _fake_scan, events, saved_modules = load_full_app_module()
        native_scan = NativePacketScan(events)
        try:
            app = module.App.__new__(module.App)
            app.runtime = {"scan_timing": {"send_every_n_frames": 1}}
            app.vdboard = types.SimpleNamespace(scan=native_scan)
            app.native_streaming = True
            app.frame_id = 0
            app.send_backoff_until_ms = 1100
            app.scan_rate = types.SimpleNamespace(tick=lambda: None)
            app.control_transport = types.SimpleNamespace(is_connected=lambda: True)
            app.udp_stream = types.SimpleNamespace(send=lambda payload, connected: True)
            app.wifi = types.SimpleNamespace(is_connected=lambda: True)
            app.sent_packets = 0
            app.failed_sends = 0
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

        self.assertEqual(native_scan.pop_packet_calls, 0)
        self.assertEqual(app.sent_packets, 0)

    def test_set_matrix_layout_persists_valid_pin_selection(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)
            response = app._handle_control_request(
                {"command": "set_matrix_layout", "analog_pins": [1], "select_pins": [1]},
                ("tcp", 0),
            )
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertEqual(response["runtime"]["matrix_layout"], {"active_rows": [1], "active_cols": [1]})

    def test_set_matrix_layout_does_not_persist_when_health_probe_fails(self):
        module, _fake_scan, events, saved_modules = load_full_app_module()
        try:
            module.config.AVAILABLE_ROWS = [1, 2]
            module.config.AVAILABLE_COLS = [1, 2]
            native_scan = NativePacketScan(events)
            app = module.App(wifi_setup_requested=False)
            app.hardware_ready = True
            app.vdboard = types.SimpleNamespace(scan=native_scan)
            app._probe_scan_health = lambda: (False, "scan_no_frames")

            response = app._handle_control_request(
                {"command": "set_matrix_layout", "analog_pins": [1, 2], "select_pins": [1, 2]},
                ("tcp", 0),
            )
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "error")
        self.assertEqual(response["message"], "matrix_layout_failed")
        self.assertEqual(response["error"], "scan_no_frames")
        self.assertEqual(response["runtime"]["matrix_layout"], {"active_rows": [1], "active_cols": [1]})

    def test_set_matrix_layout_rolls_back_when_scan_alloc_fails(self):
        module, _fake_scan, events, saved_modules = load_full_app_module()
        try:
            module.config.AVAILABLE_ROWS = [1, 2]
            module.config.AVAILABLE_COLS = [1, 2]
            failing_scan = FailingInitScan(events)
            app = module.App(wifi_setup_requested=False)
            app.hardware_ready = True
            app.vdboard = types.SimpleNamespace(scan=failing_scan)

            response = app._handle_control_request(
                {"command": "set_matrix_layout", "analog_pins": [1, 2], "select_pins": [1, 2]},
                ("tcp", 0),
            )
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "error")
        self.assertEqual(response["message"], "matrix_layout_failed")
        self.assertEqual(response["runtime"]["matrix_layout"], {"active_rows": [1], "active_cols": [1]})

    def test_set_scan_timing_accepts_arbitrary_positive_fps_and_settle_us(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)

            response = app._handle_control_request(
                {"command": "set_scan_timing", "target_fps": 75, "settle_us": 18},
                ("tcp", 0),
            )
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertEqual(response["message"], "scan_timing_updated")
        self.assertEqual(response["runtime"]["scan_timing"]["target_fps"], 75)
        self.assertEqual(response["runtime"]["scan_timing"]["settle_us"], 18)

    def test_set_scan_timing_rejects_non_positive_fps(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)

            response = app._handle_control_request(
                {"command": "set_scan_timing", "target_fps": 0},
                ("tcp", 0),
            )
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "error")
        self.assertEqual(response["message"], "scan_timing_invalid")
        self.assertEqual(response["error"], "target_fps_must_be_positive")

    def test_set_scan_timing_restarts_active_scan_with_health_probe(self):
        module, _fake_scan, events, saved_modules = load_full_app_module()
        try:
            native_scan = NativePacketScan(events)
            app = module.App(wifi_setup_requested=False)
            app.hardware_ready = True
            app.scan_ready = True
            app.vdboard = types.SimpleNamespace(scan=native_scan)
            app._probe_scan_health = lambda: (True, "")

            response = app._handle_control_request(
                {"command": "set_scan_timing", "target_fps": 90},
                ("tcp", 0),
            )
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertEqual(native_scan.init_calls[-1]["fps"], 90)
        self.assertIn("scan_stop", events)
        self.assertIn("scan_start", events)

    def test_enter_maintenance_stops_scan_and_rejects_os_writer(self):
        module, fake_scan, events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
            events.clear()

            response = app._handle_control_request(
                {"command": "enter_maintenance", "reason": "calibration"},
                ("tcp", 0),
            )
            denied = app._handle_control_request({"command": "write_os"}, ("tcp", 0))
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertEqual(response["message"], "maintenance_entered")
        self.assertEqual(app.mode, "maintenance")
        self.assertFalse(app.scan_ready)
        self.assertIsNone(app.filesystem)
        self.assertIsNone(app.calibration)
        self.assertNotIn("filesystem_api_load", events)
        self.assertNotIn("calibration_store_load", events)
        self.assertGreaterEqual(fake_scan.stop_calls, 1)
        self.assertEqual(denied["status"], "error")
        self.assertEqual(denied["message"], "requires_recovery")
        self.assertEqual(denied["next_command"], "reboot_to_recovery")

    def test_maintenance_status_does_not_load_file_or_calibration_services(self):
        module, _fake_scan, events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
            events.clear()
            app._handle_control_request({"command": "enter_maintenance"}, ("tcp", 0))

            response = app._handle_control_request({"command": "maintenance_status"}, ("tcp", 0))
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertIsNone(app.filesystem)
        self.assertIsNone(app.calibration)
        self.assertNotIn("filesystem_api_load", events)
        self.assertNotIn("calibration_store_load", events)

    def test_maintenance_file_command_loads_only_filesystem_service(self):
        module, _fake_scan, events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
            events.clear()
            app._handle_control_request({"command": "enter_maintenance"}, ("tcp", 0))

            response = app._handle_control_request({"command": "file_list", "scope": "user"}, ("tcp", 0))
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertIsNotNone(app.filesystem)
        self.assertIsNone(app.calibration)
        self.assertIn("filesystem_api_load", events)
        self.assertNotIn("calibration_store_load", events)

    def test_findme_retries_quickly_after_gateway_timeout_or_lost_connection(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module(
            runtime_override={
                "server": {"host": "192.168.1.153", "tcp_port": 22345, "udp_port": 13250, "source": "findme", "gateway_id": "local-gateway"},
                "findme": {"host": "192.168.1.153", "gateway_id": "local-gateway", "last_error": "findme_timeout:[Errno 116] ETIMEDOUT"},
            }
        )
        try:
            app = module.App(wifi_setup_requested=False)
            app.wifi.connected = True
            app.control_transport = types.SimpleNamespace(
                is_connected=lambda: False,
                findme_status=lambda: {
                    "state": "gateway_lost",
                    "host": "192.168.1.153",
                    "last_error": "connect_failed:[Errno 116] ETIMEDOUT",
                    "connected": False,
                },
                reconfigure=lambda: None,
            )
            app.udp_stream = types.SimpleNamespace(reconfigure=lambda: None)
            app.last_findme_ms = 0
            module.time.ticks_diff = lambda now, then: now - then

            skipped = app._service_findme(4999)
            retried = app._service_findme(5000)
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertFalse(skipped)
        self.assertTrue(retried)
        self.assertEqual(app.wifi.findme_results, ["retry"])

    def test_recovery_check_and_write_require_explicit_resource_release(self):
        writer_calls = []

        class FakeWriter:
            def __init__(self, target, root_dir=".", logger=None, progress=None):
                self.target = target
                self.progress = progress
                writer_calls.append(("init", target, root_dir))

            def check_release(self, release_url):
                writer_calls.append(("check", release_url))
                return {
                    "status": "ok",
                    "message": "recovery_release_checked",
                    "latest_version": "v-recovery-next",
                    "manifest_url": "https://example.com/recovery/manifest.json",
                }

            def write_release(self, release_url):
                writer_calls.append(("write", release_url))
                if self.progress:
                    self.progress({
                        "phase": "complete",
                        "operation": "write_recovery",
                        "version": "v-recovery-next",
                        "total_files": 2,
                        "written_files": 2,
                        "skipped_files": 0,
                        "current_file": "",
                    })
                return {
                    "status": "ok",
                    "message": "recovery_write_complete",
                    "version": "v-recovery-next",
                    "downloaded_files": 2,
                    "skipped_files": 0,
                    "deleted_files": 0,
                    "reboot_required": True,
                }

        module, _fake_scan, _events, saved_modules = load_full_app_module(
            runtime_override={"update": {"release_url": "https://example.com/latest.json"}}
        )
        saved_update_writer = sys.modules.get("update_writer")
        sys.modules["update_writer"] = types.SimpleNamespace(ManifestTargetWriter=FakeWriter)
        try:
            app = module.App(wifi_setup_requested=False)
            app.wifi.connected = True
            check_rejected = app._handle_control_request({"command": "check_recovery_release"}, ("tcp", 0))
            write_rejected = app._handle_control_request({"command": "write_recovery"}, ("tcp", 0))
            released = app._handle_control_request({"command": "release_recovery_resources"}, ("tcp", 0))
            checked = app._handle_control_request({"command": "check_recovery_release"}, ("tcp", 0))
            written = app._handle_control_request({"command": "write_recovery"}, ("tcp", 0))
            status = app._status()
        finally:
            if saved_update_writer is None:
                sys.modules.pop("update_writer", None)
            else:
                sys.modules["update_writer"] = saved_update_writer
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(check_rejected["status"], "error")
        self.assertEqual(check_rejected["message"], "recovery_resources_required")
        self.assertEqual(check_rejected["next_command"], "release_recovery_resources")
        self.assertEqual(write_rejected["status"], "error")
        self.assertEqual(write_rejected["message"], "recovery_resources_required")
        self.assertEqual(write_rejected["next_command"], "release_recovery_resources")
        self.assertEqual(released["message"], "recovery_resources_released")
        self.assertEqual(released["mode"], "normal")
        self.assertTrue(released["recovery_update_low_resource"])
        self.assertEqual(checked["message"], "recovery_release_checked")
        self.assertEqual(written["message"], "recovery_write_complete")
        self.assertEqual(written["update_state"]["operation"], "write_recovery")
        self.assertEqual(status["system"]["recovery_version"], "v-recovery-next")
        self.assertTrue(status["recovery_update_low_resource"])
        self.assertEqual(app.mode, "normal")
        self.assertEqual(app.control_transport.status_payloads, [])
        self.assertEqual(len(app.control_transport.progress_payloads), 1)
        progress_payload, connected = app.control_transport.progress_payloads[0]
        self.assertTrue(connected)
        self.assertEqual(progress_payload["message"], "recovery_write_complete")
        self.assertEqual(progress_payload["mode"], "normal")
        self.assertEqual(progress_payload["device_uid"], "UID123")
        self.assertEqual(progress_payload["update_state"]["operation"], "write_recovery")
        self.assertEqual(progress_payload["update_state"]["phase"], "done")
        self.assertNotIn("runtime", progress_payload)
        self.assertEqual(app.control_transport.flush_calls, [4096])
        self.assertEqual(
            writer_calls,
            [
                ("init", "recovery", "."),
                ("check", "https://example.com/latest.json"),
                ("init", "recovery", "."),
                ("write", "https://example.com/latest.json"),
            ],
        )

    def test_release_recovery_resources_enters_low_resource_mode_without_maintenance(self):
        class FakeWriter:
            def __init__(self, target, root_dir=".", logger=None, progress=None):
                self.progress = progress

            def write_release(self, release_url):
                if self.progress:
                    self.progress({
                        "phase": "complete",
                        "operation": "write_recovery",
                        "version": "v-recovery-next",
                        "total_files": 1,
                        "written_files": 1,
                        "skipped_files": 0,
                        "current_file": "",
                    })
                return {
                    "status": "ok",
                    "message": "recovery_write_complete",
                    "version": "v-recovery-next",
                    "downloaded_files": 1,
                    "skipped_files": 0,
                    "deleted_files": 0,
                    "reboot_required": True,
                }

        module, _fake_scan, _events, saved_modules = load_full_app_module(
            runtime_override={"update": {"release_url": "https://example.com/latest.json"}},
            enable_led=True,
            enable_battery=True,
            enable_imu=True,
        )
        saved_update_writer = sys.modules.get("update_writer")
        sys.modules["update_writer"] = types.SimpleNamespace(ManifestTargetWriter=FakeWriter)
        try:
            app = module.App(wifi_setup_requested=False)
            app.wifi.connected = True
            app.setup()
            self.assertIsNotNone(app.led)
            self.assertIsNotNone(app.battery)
            self.assertIsNotNone(app.imu)
            self.assertIsNotNone(app.udp_stream)
            self.assertIsNotNone(app.time_sync)
            released = app._handle_control_request({"command": "release_recovery_resources"}, ("tcp", 0))

            written = app._handle_control_request({"command": "write_recovery"}, ("tcp", 0))
            memory_status = app._handle_control_request({"command": "memory_status"}, ("tcp", 0))
        finally:
            if saved_update_writer is None:
                sys.modules.pop("update_writer", None)
            else:
                sys.modules["update_writer"] = saved_update_writer
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(released["status"], "ok")
        self.assertEqual(released["message"], "recovery_resources_released")
        self.assertEqual(released["mode"], "normal")
        self.assertTrue(released["recovery_update_low_resource"])
        self.assertEqual(app.mode, "normal")
        self.assertTrue(app.recovery_update_low_resource)
        self.assertEqual(written["status"], "ok")
        self.assertTrue(memory_status["recovery_update_low_resource"])
        self.assertIsNotNone(app.control_transport)
        self.assertIsNone(app.udp_stream)
        self.assertIsNone(app.time_sync)
        self.assertIsNone(app.scan_rate)
        self.assertIsNone(app.udp_rate)
        self.assertIsNone(app.led)
        self.assertIsNone(app.battery)
        self.assertIsNone(app.imu)
        self.assertIsNone(app.vdboard)

    def test_maintenance_sample_cell_runs_bounded_scan_and_stops_afterwards(self):
        module, fake_scan, events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
            events.clear()
            app._handle_control_request({"command": "enter_maintenance"}, ("tcp", 0))
            fake_scan.sample_cell_mv = lambda analog_pin, select_pin, duration_ms: 123.5
            gc_calls = []
            module.gc.collect = lambda: gc_calls.append("collect")

            response = app._handle_control_request(
                {
                    "command": "calibration_sample_cell",
                    "analog_pin": 1,
                    "select_pin": 1,
                    "level": 2.5,
                    "duration_ms": 3000,
                },
                ("tcp", 0),
            )
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertEqual(response["message"], "calibration_cell_sampled")
        self.assertEqual(response["avg_mv"], 123.5)
        self.assertEqual(app.mode, "maintenance")
        self.assertFalse(app.scan_ready)
        self.assertGreaterEqual(fake_scan.start_calls, 2)
        self.assertGreaterEqual(fake_scan.stop_calls, 2)
        self.assertEqual(gc_calls, ["collect"])
        self.assertIn("calibration_store_load", events)
        self.assertNotIn("filesystem_api_load", events)

    def test_maintenance_sample_all_reads_native_frame_without_frame_protocol_decoder(self):
        frame = struct.pack(
            "<IIHHHH4H",
            1,
            100,
            2,
            2,
            4,
            1,
            100,
            200,
            300,
            400,
        )
        module, _fake_scan, events, saved_modules = load_full_app_module(
            runtime_override={"matrix_layout": {"active_rows": [1, 2], "active_cols": [3, 4]}}
        )
        try:
            module.config.AVAILABLE_ROWS = [1, 2]
            module.config.AVAILABLE_COLS = [3, 4]
            clock = [0]
            module.time.ticks_ms = lambda: clock.__setitem__(0, clock[0] + 1) or clock[0]
            module.time.ticks_add = lambda now, delta: now + delta
            module.time.ticks_diff = lambda future, now: future - now
            module.time.sleep_ms = lambda _ms: None
            app = module.App(wifi_setup_requested=False)
            app.setup()
            app.vdboard = types.SimpleNamespace(scan=FrameSampleScan(frame, events))
            app._handle_control_request({"command": "enter_maintenance"}, ("tcp", 0))

            response = app._handle_control_request(
                {"command": "calibration_sample_all", "level": 1.25, "duration_ms": 3},
                ("tcp", 0),
            )
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertEqual(response["message"], "calibration_all_sampled")
        self.assertEqual(response["cells"], 4)
        self.assertIn("calibration_store_load", events)
        self.assertNotIn("filesystem_api_load", events)
        calibration_points = [item for item in events if isinstance(item, tuple) and item[0] == "calibration_set_point"]
        self.assertEqual(len(calibration_points), 4)
        self.assertEqual(calibration_points[0][1], (1, 3, 1.25, 100.0))
        self.assertEqual(calibration_points[-1][1], (2, 4, 1.25, 400.0))

    def test_maintenance_sample_all_rejects_low_heap_before_starting_scan(self):
        module, _fake_scan, events, saved_modules = load_full_app_module()
        had_mem_free = hasattr(module.gc, "mem_free")
        old_mem_free = getattr(module.gc, "mem_free", None)
        try:
            module.gc.mem_free = lambda: 1024
            module.config.CALIBRATION_MIN_HEAP_FREE = 4096
            app = module.App(wifi_setup_requested=False)
            app.setup()
            events.clear()
            app._handle_control_request({"command": "enter_maintenance"}, ("tcp", 0))

            response = app._handle_control_request(
                {"command": "calibration_sample_all", "level": 1.0, "duration_ms": 3000},
                ("tcp", 0),
            )
        finally:
            if had_mem_free:
                module.gc.mem_free = old_mem_free
            elif hasattr(module.gc, "mem_free"):
                delattr(module.gc, "mem_free")
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "error")
        self.assertEqual(response["message"], "calibration_heap_low")
        self.assertEqual(response["heap_free"], 1024)
        self.assertNotIn("scan_start", events)

    def test_legacy_calibration_flow_commands_are_removed(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)
            responses = [
                app._handle_control_request({"command": command}, ("tcp", 0))
                for command in (
                    "enter_calibration_mode",
                    "start_calibration",
                    "calibrate_all",
                    "end_calibration",
                )
            ]
            status = app._status()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual([response["message"] for response in responses], ["unknown_command"] * 4)
        self.assertNotIn("calibration_mode", status)

    def test_maintenance_rejects_normal_runtime_commands(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
            app._handle_control_request({"command": "enter_maintenance"}, ("tcp", 0))

            response = app._handle_control_request(
                {"command": "set_matrix_layout", "analog_pins": [1], "select_pins": [1]},
                ("tcp", 0),
            )
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "error")
        self.assertEqual(response["message"], "maintenance_command_disabled")

    def test_set_logging_updates_runtime_and_logger_in_normal_mode(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)
            response = app._handle_control_request(
                {"command": "set_logging", "enabled": False, "capacity": "extended"},
                ("tcp", 0),
            )
            status = app._status()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertEqual(response["message"], "logging_updated")
        self.assertFalse(response["logging"]["enabled"])
        self.assertEqual(response["logging"]["capacity"], "extended")
        self.assertEqual(response["logging"]["max_bytes"], 65536)
        self.assertEqual(app.config_store.runtime["logging"]["enabled"], False)
        self.assertEqual(status["logging"]["capacity"], "extended")

    def test_set_logging_is_allowed_in_maintenance_mode(self):
        module, _fake_scan, _events, saved_modules = load_full_app_module()
        try:
            app = module.App(wifi_setup_requested=False)
            app.setup()
            app._handle_control_request({"command": "enter_maintenance"}, ("tcp", 0))

            response = app._handle_control_request(
                {"command": "set_logging", "enabled": True, "capacity": "extended"},
                ("tcp", 0),
            )
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(response["status"], "ok")
        self.assertEqual(response["message"], "logging_updated")
        self.assertEqual(response["logging"]["capacity"], "extended")


if __name__ == "__main__":
    unittest.main()
