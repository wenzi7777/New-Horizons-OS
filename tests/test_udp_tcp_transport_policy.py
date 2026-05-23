import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


class UDPTCPTransportPolicyTest(unittest.TestCase):
    def test_device_uses_tcp_control_and_udp_stream_modules(self):
        app_source = (PROJECT_ROOT / "device/os/app_core.py").read_text(encoding="utf-8")
        recovery_source = (PROJECT_ROOT / "device/recovery/recovery_app.py").read_text(encoding="utf-8")

        self.assertIn("from tcp_control import TCPControlTransport", app_source)
        self.assertIn("from udp_stream import UDPStreamTransport", app_source)
        self.assertIn("from tcp_control import TCPControlTransport", recovery_source)
        self.assertNotIn("from mqtt_transport import MQTTTransport", app_source)
        self.assertNotIn("from mqtt_transport import MQTTTransport", recovery_source)

    def test_udp_stream_is_best_effort_sendto(self):
        source = (PROJECT_ROOT / "device/os/udp_stream.py").read_text(encoding="utf-8")

        self.assertIn("sendto(payload, self.addr)", source)
        self.assertNotIn("MQTTClient", source)
        self.assertNotIn("publish", source)

    def test_tcp_control_uses_jsonl_request_id_results(self):
        for relative_path in ("device/os/tcp_control.py", "device/recovery/tcp_control.py"):
            with self.subTest(path=relative_path):
                source = (PROJECT_ROOT / relative_path).read_text(encoding="utf-8")
                self.assertIn('data.get("type") == "command"', source)
                self.assertIn('"request_id"', source)
                self.assertIn('"type": message_type', source)
                self.assertIn('("tcp", 0)', source)


if __name__ == "__main__":
    unittest.main()
