import importlib.util
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def load_module(relative_path):
    module_path = PROJECT_ROOT / relative_path
    spec = importlib.util.spec_from_file_location("tcp_control_under_test", module_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeLogger:
    def __init__(self):
        self.infos = []
        self.warns = []

    def info(self, message):
        self.infos.append(message)

    def warn(self, message):
        self.warns.append(message)


class TCPControlLoggingTest(unittest.TestCase):
    def test_tcp_command_result_echoes_command_and_request_id(self):
        module = load_module("device/os/tcp_control.py")
        logger = FakeLogger()
        transport = module.TCPControlTransport(lambda: {"server": {"host": "127.0.0.1", "tcp_port": 1}}, "ABC", logger)
        sent = []
        transport.sock = type("Sock", (), {"send": lambda _self, data: sent.append(data) or len(data)})()
        transport.sock_key = ("127.0.0.1", 1, "ABC")

        response = transport._annotate_response(
            {"command": "status", "request_id": "req-1"},
            {"status": "ok", "message": "status"},
            "status",
        )
        transport.publish_result(response, True)

        self.assertIn(b'"type": "result"', sent[-1])
        self.assertIn(b'"request_id": "req-1"', sent[-1])
        self.assertIn(b'"command": "status"', sent[-1])


if __name__ == "__main__":
    unittest.main()
