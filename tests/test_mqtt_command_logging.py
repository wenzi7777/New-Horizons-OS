import importlib
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def import_transport(layer):
    root = REPO_ROOT / "device" / layer
    sys.path.insert(0, str(root))
    for name in list(sys.modules):
        if name == "mqtt_transport" or name.startswith("umqtt"):
            sys.modules.pop(name)
    try:
        return importlib.import_module("mqtt_transport")
    finally:
        sys.path.pop(0)


class FakeClient:
    def __init__(self, callback):
        self.callback = callback
        self.published = []

    def check_msg(self):
        self.callback(b"newhorizons/v1/device/ABC/cmd", b'{"command":"check_os_release"}')

    def publish(self, topic, payload, qos=0):
        self.published.append((topic, payload, qos))


class FakeLogger:
    def __init__(self):
        self.infos = []
        self.warns = []

    def info(self, message):
        self.infos.append(message)

    def warn(self, message):
        self.warns.append(message)


class MQTTCommandLoggingTest(unittest.TestCase):
    def _transport(self, module, logger):
        runtime = {
            "transport": {"mode": "mqtt", "topic_namespace": "newhorizons/v1"},
            "mqtt": {"host": "127.0.0.1", "port": 1883, "tls": False},
        }
        transport = module.MQTTTransport(lambda: runtime, "ABC", logger)
        transport.client = FakeClient(transport._on_message)
        transport.client_key = ("127.0.0.1", 1883, False, None, None, "newhorizons/v1", "ABC")
        return transport

    def test_mqtt_commands_log_received_and_done(self):
        for layer in ("recovery", "os"):
            with self.subTest(layer=layer):
                module = import_transport(layer)
                logger = FakeLogger()
                transport = self._transport(module, logger)

                transport.poll(True, lambda request, _addr: {"status": "ok", "message": request["command"]})

                self.assertTrue(any("mqtt_command_received command=check_os_release" in item for item in logger.infos))
                self.assertTrue(any("mqtt_command_done command=check_os_release status=ok" in item for item in logger.infos))

    def test_mqtt_command_handler_exception_is_reported_as_result(self):
        for layer in ("recovery", "os"):
            with self.subTest(layer=layer):
                module = import_transport(layer)
                logger = FakeLogger()
                transport = self._transport(module, logger)

                transport.poll(True, lambda _request, _addr: (_ for _ in ()).throw(RuntimeError("boom")))

                self.assertTrue(any("mqtt_command_failed command=check_os_release" in item for item in logger.warns))
                self.assertTrue(transport.client.published)


if __name__ == "__main__":
    unittest.main()
