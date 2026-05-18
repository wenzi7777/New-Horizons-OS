import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
MINIMAL_APP_PATH = REPO_ROOT / "device" / "channels" / "minimal" / "files" / "app_minimal.py"
FULL_APP_PATH = REPO_ROOT / "device" / "channels" / "full" / "files" / "app.py"


def _load_module(module_name, module_path, injected_modules):
    old_modules = {}
    for name, module in injected_modules.items():
        old_modules[name] = sys.modules.get(name)
        sys.modules[name] = module

    try:
        spec = importlib.util.spec_from_file_location(module_name, module_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.time.ticks_add = lambda now, delta: now + delta
        return module
    finally:
        for name, old in old_modules.items():
            if old is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = old


def load_minimal_app_module():
    return _load_module(
        "app_minimal_reboot_timing_test",
        MINIMAL_APP_PATH,
        {
            "machine": types.SimpleNamespace(reset=lambda: None),
            "immutable_config": types.SimpleNamespace(
                FIRMWARE_NAME="New Horizons OS",
                LOG_PATH="device_state/logs/device.log",
                DEVICE_STATE_DIR="device_state",
                DEFAULT_CONTROL_PORT=22345,
                STATUS_ANNOUNCE_INTERVAL_MS=2000,
                DEFAULT_MANIFESTS={
                    "minimal": "https://example.com/minimal/manifest.json",
                    "full": "https://example.com/full/manifest.json",
                },
            ),
            "device_identity": types.SimpleNamespace(
                get_device_id=lambda: 0x12345678,
                get_device_uid=lambda: "UID123",
                get_device_name=lambda default: default,
            ),
            "device_logging": types.SimpleNamespace(DeviceLogger=lambda path: None),
            "filesystem_api": types.SimpleNamespace(FilesystemAPI=lambda root: None),
            "mqtt_transport": types.SimpleNamespace(
                MQTTTransport=lambda *args, **kwargs: types.SimpleNamespace(
                    poll=lambda *poll_args, **poll_kwargs: None,
                    publish_status=lambda *status_args, **status_kwargs: True,
                )
            ),
            "runtime_config": types.SimpleNamespace(RuntimeConfigStore=lambda root: None),
            "udp_control": types.SimpleNamespace(UDPControlServer=lambda port, logger=None: None),
            "update_manager": types.SimpleNamespace(UpdateManager=lambda *args, **kwargs: None),
            "wifi_manager": types.SimpleNamespace(WiFiManager=lambda *args, **kwargs: None),
        },
    )


def load_full_app_module():
    fake_scan = types.SimpleNamespace(start=lambda: None, service=lambda: None, pop_frame_mv=lambda: None)
    fake_vdboard = types.SimpleNamespace(scan=fake_scan)
    return _load_module(
        "app_full_reboot_timing_test",
        FULL_APP_PATH,
        {
            "machine": types.SimpleNamespace(reset=lambda: None),
            "config": types.SimpleNamespace(
                DEVICE_NAME="New Horizons OS",
                ENABLE_IMU=False,
                ENABLE_BATTERY=False,
                ENABLE_LED=False,
                USE_PACKET_BUFFER=False,
                DEVICE_STATE_DIR="device_state",
                LOG_PATH="device_state/logs/device.log",
                UDP_CONTROL_PORT=22345,
                STATUS_ANNOUNCE_INTERVAL_MS=2000,
                TARGET_FPS=60,
                IMU_RATE_HZ=60,
                BATTERY_RATE_HZ=2,
                LED_RATE_HZ=20,
                GC_EVERY_N_FRAMES=0,
                ACTIVE_ROWS=[1],
                ACTIVE_COLS=[1],
                SEND_EVERY_N_FRAMES=1,
                PACKET_BUFFER_SIZE=1,
                PACKET_BUFFER_DROP_OLDEST=False,
                PRINT_PIN_CONFLICTS=False,
            ),
            "board_pins": types.SimpleNamespace(validate_pins=lambda: {}),
            "vdboard": fake_vdboard,
            "calibration_store": types.SimpleNamespace(CalibrationStore=lambda root: None),
            "device_identity": types.SimpleNamespace(
                get_device_id=lambda: 0x12345678,
                get_device_uid=lambda: "UID123",
                get_device_name=lambda default: default,
            ),
            "device_logging": types.SimpleNamespace(DeviceLogger=lambda path: None),
            "filesystem_api": types.SimpleNamespace(FilesystemAPI=lambda root: None),
            "filter_engine": types.SimpleNamespace(FilterChain=lambda **kwargs: None),
            "frame_protocol": types.SimpleNamespace(decode_scan_frame=lambda payload: {}),
            "mqtt_transport": types.SimpleNamespace(
                MQTTTransport=lambda *args, **kwargs: types.SimpleNamespace(
                    poll=lambda *poll_args, **poll_kwargs: None,
                    publish_raw=lambda *raw_args, **raw_kwargs: True,
                    publish_status=lambda *status_args, **status_kwargs: True,
                )
            ),
            "packet": types.SimpleNamespace(PacketBuilder=lambda: None),
            "packet_buffer": types.SimpleNamespace(PacketBuffer=lambda **kwargs: None),
            "runtime_config": types.SimpleNamespace(RuntimeConfigStore=lambda root: None),
            "time_sync": types.SimpleNamespace(TimeSync=lambda servers: types.SimpleNamespace(status=lambda: {"synced": False})),
            "udp_control": types.SimpleNamespace(UDPControlServer=lambda port, logger=None: None),
            "update_manager": types.SimpleNamespace(UpdateManager=lambda *args, **kwargs: None),
            "utils": types.SimpleNamespace(RateCounter=lambda interval: None),
            "wifi_manager": types.SimpleNamespace(WiFiManager=lambda *args, **kwargs: None),
        },
    )


class UpdateRebootTimingTests(unittest.TestCase):
    def test_minimal_app_waits_for_update_applied_before_rebooting(self):
        module = load_minimal_app_module()
        app = module.MinimalApp.__new__(module.MinimalApp)
        app.pending_channel_switch = None
        app.reboot_required = False
        app.reboot_deadline_ms = None

        app._handle_update_result(
            {"status": "ok", "message": "update_progress", "reboot_required": True},
            5000,
        )

        self.assertFalse(app.reboot_required)
        self.assertIsNone(app.reboot_deadline_ms)

        app._handle_update_result(
            {"status": "ok", "message": "update_applied", "reboot_required": True},
            5000,
        )

        self.assertTrue(app.reboot_required)
        self.assertEqual(app.reboot_deadline_ms, 6200)

    def test_full_app_waits_for_update_applied_before_rebooting(self):
        module = load_full_app_module()
        app = module.App.__new__(module.App)
        app.reboot_required = False
        app.reboot_deadline_ms = None

        app._handle_update_result(
            {"status": "ok", "message": "update_progress", "reboot_required": True},
            9000,
        )

        self.assertFalse(app.reboot_required)
        self.assertIsNone(app.reboot_deadline_ms)

        app._handle_update_result(
            {"status": "ok", "message": "update_applied", "reboot_required": True},
            9000,
        )

        self.assertTrue(app.reboot_required)
        self.assertEqual(app.reboot_deadline_ms, 10200)


if __name__ == "__main__":
    unittest.main()
