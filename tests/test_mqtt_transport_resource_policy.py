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

    def test_os_raw_sensor_stream_uses_qos0(self):
        source = (PROJECT_ROOT / "device/os/mqtt_transport.py").read_text(encoding="utf-8")

        self.assertIn("publish_raw(self, payload, wifi_connected)", source)
        self.assertIn('self.client is None', source)
        self.assertIn('self._publish_bytes(self._topic_bytes("raw"), payload, qos=0)', source)
        self.assertIn('self._publish_json(self._topic_bytes("status"), payload, wifi_connected, qos=1)', source)
        self.assertIn('self._publish_json(self._topic_bytes("result"), payload, wifi_connected, qos=1)', source)
        self.assertIn("def _topic_bytes(self, kind):", source)


if __name__ == "__main__":
    unittest.main()
