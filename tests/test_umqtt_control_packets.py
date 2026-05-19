import importlib.util
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def load_simple_module(relative_path):
    module_path = PROJECT_ROOT / relative_path
    spec = importlib.util.spec_from_file_location(
        "test_{}_simple".format(relative_path.parts[1]),
        module_path,
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def encode_remaining_length(value):
    encoded = bytearray()
    while True:
        byte = value % 128
        value //= 128
        if value:
            byte |= 0x80
        encoded.append(byte)
        if not value:
            return bytes(encoded)


def publish_packet(topic, payload):
    variable = len(topic).to_bytes(2, "big") + topic + payload
    return b"\x30" + encode_remaining_length(len(variable)) + variable


class FakeSocket:
    def __init__(self, incoming):
        self.incoming = bytearray(incoming)
        self.pos = 0
        self.writes = []
        self.blocking = True

    def read(self, size):
        if self.pos >= len(self.incoming):
            return None
        data = bytes(self.incoming[self.pos:self.pos + size])
        self.pos += len(data)
        return data

    def write(self, data, size=None):
        if isinstance(data, int):
            data = bytes([data])
        if size is not None:
            data = data[:size]
        self.writes.append(bytes(data))
        return len(data)

    def setblocking(self, blocking):
        self.blocking = blocking


class UmqttControlPacketTest(unittest.TestCase):
    def test_check_msg_drains_puback_before_next_publish(self):
        for relative_path in (
            Path("device/recovery/umqtt/simple.py"),
            Path("device/os/umqtt/simple.py"),
        ):
            with self.subTest(path=str(relative_path)):
                simple = load_simple_module(relative_path)
                topic = b"newhorizons/v1/device/test/cmd"
                payload = b'{"command":"status"}'
                sock = FakeSocket(
                    b"\x40\x02\x12\x34" + publish_packet(topic, payload)
                )
                received = []

                client = simple.MQTTClient("client", "broker")
                client.sock = sock
                client.set_callback(
                    lambda received_topic, received_payload: received.append(
                        (received_topic, received_payload)
                    )
                )

                self.assertEqual(client.check_msg(), 0x40)
                self.assertEqual(sock.pos, 4)

                self.assertEqual(client.check_msg(), 0x30)
                self.assertEqual(received, [(topic, payload)])


if __name__ == "__main__":
    unittest.main()
