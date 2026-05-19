import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


class MQTTTransportResourcePolicyTest(unittest.TestCase):
    def test_transport_uses_simple_client_so_reconnect_is_bounded(self):
        for relative_path in (
            "device/recovery/mqtt_transport.py",
            "device/os/mqtt_transport.py",
        ):
            with self.subTest(path=relative_path):
                source = (PROJECT_ROOT / relative_path).read_text(encoding="utf-8")
                self.assertIn("from umqtt.simple import MQTTClient", source)
                self.assertNotIn("from umqtt.robust import MQTTClient", source)


if __name__ == "__main__":
    unittest.main()
