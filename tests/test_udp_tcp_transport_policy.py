import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


class UDPTCPTransportPolicyTest(unittest.TestCase):
    def test_normal_os_uses_udp_nhcp_control_and_udp_stream_modules(self):
        app_source = (PROJECT_ROOT / "device/os/app_core.py").read_text(encoding="utf-8")
        recovery_source = (PROJECT_ROOT / "device/recovery/recovery_app.py").read_text(encoding="utf-8")

        self.assertIn("from udp_control import UDPControlTransport", app_source)
        self.assertIn("from udp_stream import UDPStreamTransport", app_source)
        self.assertNotIn("from tcp_control import TCPControlTransport", app_source)
        self.assertIn("from tcp_control import TCPControlTransport", recovery_source)
        self.assertNotIn("from mqtt_transport import MQTTTransport", app_source)
        self.assertNotIn("from mqtt_transport import MQTTTransport", recovery_source)

    def test_udp_stream_is_best_effort_sendto(self):
        source = (PROJECT_ROOT / "device/os/udp_stream.py").read_text(encoding="utf-8")

        self.assertIn("sendto(payload, self.addr)", source)
        self.assertNotIn("MQTTClient", source)
        self.assertNotIn("publish", source)

    def test_normal_os_udp_control_uses_nhcp_tlv_request_id_results(self):
        source = (PROJECT_ROOT / "device/os/udp_control.py").read_text(encoding="utf-8")

        self.assertIn("import nhcp", source)
        self.assertIn('frame.get("type") == "command"', source)
        self.assertIn('"request_id"', source)
        self.assertIn('("udp", 0)', source)
        self.assertNotIn("import json", source)
        self.assertNotIn("json.loads", source)
        self.assertNotIn("json.dumps", source)

    def test_findme_uses_nhcp_tlv_not_json(self):
        source = (PROJECT_ROOT / "device/os/findme.py").read_text(encoding="utf-8")

        self.assertIn("import nhcp", source)
        self.assertIn('"findme_discover"', source)
        self.assertIn('"findme_offer"', source)
        self.assertNotIn("import json", source)
        self.assertNotIn("json.loads", source)
        self.assertNotIn("json.dumps", source)

    def test_recovery_keeps_tcp_jsonl_control_for_ota(self):
        source = (PROJECT_ROOT / "device/recovery/tcp_control.py").read_text(encoding="utf-8")

        self.assertIn('data.get("type") == "command"', source)
        self.assertIn('"request_id"', source)
        self.assertIn('"type": message_type', source)
        self.assertIn('("tcp", 0)', source)


if __name__ == "__main__":
    unittest.main()
