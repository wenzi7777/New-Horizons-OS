import importlib.util
import struct
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_host_ui():
    path = REPO_ROOT / "host_ui.py"
    spec = importlib.util.spec_from_file_location("host_ui_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class HostUiCoreTests(unittest.TestCase):
    def test_service_control_announcement_populates_registry_without_manual_add(self):
        module = load_host_ui()
        service = module.HostUiService(control_local_port=0)
        service.control.sock.close()

        class FakeControl:
            def poll_announcements(self, handler, timeout=0.2, max_packets=32):
                handler(
                    ("192.168.1.88", 22345),
                    {
                        "status": "ok",
                        "device_id": "0xCCDDEEFF",
                        "device_uid": "AABBCCDDEEFF",
                        "device_name": "New Horizons OS-DDEEFF",
                    },
                )

        service.control = FakeControl()

        service._ingest_control_announcements()

        devices = service.registry.list_devices()
        self.assertEqual(len(devices), 1)
        self.assertEqual(devices[0]["host"], "192.168.1.88")
        self.assertEqual(devices[0]["device_uid"], "AABBCCDDEEFF")

    def test_registry_merges_packet_and_status_into_mac_key(self):
        module = load_host_ui()
        registry = module.DeviceRegistry()

        registry.upsert_packet(
            ("192.168.1.50", 5005),
            {
                "device_id": "0xCCDDEEFF",
                "device_name": "",
                "frame_id": 7,
                "timestamp_ms": 123,
                "matrix": [1.0, 2.0],
                "rows": 1,
                "cols": 2,
            },
        )
        record = registry.apply_status(
            "192.168.1.50",
            {
                "device_id": "0xCCDDEEFF",
                "device_uid": "AABBCCDDEEFF",
                "device_name": "New Horizons OS-DDEEFF",
            },
        )

        self.assertEqual(record["key"], "AABBCCDDEEFF")
        self.assertEqual(record["packet"]["frame_id"], 7)
        self.assertEqual(registry.get("AABBCCDDEEFF")["host"], "192.168.1.50")

    def test_binary_packet_parser_extracts_float_matrix(self):
        module = load_host_ui()
        payload = struct.pack("<ffff", 1.5, 2.5, 3.5, 4.5)
        packet = struct.pack("<HBBIIIH", 0xA55A, 1, 0, 0xCCDDEEFF, 10, 20, len(payload)) + payload

        parsed = module.parse_binary_packet(packet)

        self.assertEqual(parsed["device_id"], "0xCCDDEEFF")
        self.assertEqual(parsed["frame_id"], 10)
        self.assertEqual(parsed["matrix"], [1.5, 2.5, 3.5, 4.5])


if __name__ == "__main__":
    unittest.main()
