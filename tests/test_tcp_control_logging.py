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
        module = load_module("device/recovery/tcp_control.py")
        logger = FakeLogger()
        transport = module.TCPControlTransport(lambda: {"server": {"host": "127.0.0.1", "tcp_port": 1}}, "ABC", logger)
        sent = []
        transport.sock = type("Sock", (), {"send": lambda _self, data: sent.append(bytes(data)) or len(data)})()
        transport.sock_key = ("127.0.0.1", 1, "ABC")
        transport.hello_sent = True

        response = transport._annotate_response(
            {"command": "status", "request_id": "req-1"},
            {"status": "ok", "message": "status"},
            "status",
        )
        transport.publish_result(response, True)
        transport.flush()

        self.assertIn(b'"type":"result"', sent[-1])
        self.assertIn(b'"request_id":"req-1"', sent[-1])
        self.assertIn(b'"command":"status"', sent[-1])

    def test_tcp_send_retries_eagain_without_closing_socket(self):
        class EagainThenSendSocket:
            def __init__(self):
                self.calls = 0
                self.closed = False
                self.payloads = []

            def send(self, data):
                self.calls += 1
                if self.calls == 1:
                    raise OSError(11)
                self.payloads.append(bytes(data))
                return len(data)

            def close(self):
                self.closed = True

        for relative_path in ("device/recovery/tcp_control.py",):
            with self.subTest(path=relative_path):
                module = load_module(relative_path)
                logger = FakeLogger()
                transport = module.TCPControlTransport(lambda: {"server": {"host": "127.0.0.1", "tcp_port": 1}}, "ABC", logger)
                sock = EagainThenSendSocket()
                transport.sock = sock
                transport.sock_key = ("127.0.0.1", 1, "ABC")
                transport.hello_sent = True

                self.assertTrue(transport.publish_status({"message": "status"}, True))
                self.assertFalse(transport.flush())
                self.assertEqual(sock.calls, 1)
                self.assertFalse(sock.closed)
                self.assertIs(transport.sock, sock)
                self.assertTrue(transport.flush())
                self.assertIn(b'"type":"status"', sock.payloads[-1])

    def test_tcp_send_handles_partial_writes(self):
        class PartialSocket:
            def __init__(self):
                self.parts = []

            def send(self, data):
                chunk = bytes(data[: max(1, len(data) // 2)])
                self.parts.append(chunk)
                return len(chunk)

        for relative_path in ("device/recovery/tcp_control.py",):
            with self.subTest(path=relative_path):
                module = load_module(relative_path)
                logger = FakeLogger()
                transport = module.TCPControlTransport(lambda: {"server": {"host": "127.0.0.1", "tcp_port": 1}}, "ABC", logger)
                sock = PartialSocket()
                transport.sock = sock
                transport.sock_key = ("127.0.0.1", 1, "ABC")
                transport.hello_sent = True

                self.assertTrue(transport.publish_result({"message": "ok"}, True))
                for _ in range(10):
                    if transport.flush():
                        break
                joined = b"".join(sock.parts)
                self.assertIn(b'"type":"result"', joined)
                self.assertTrue(joined.endswith(b"\n"))

    def test_status_announce_is_coalesced_but_result_is_kept(self):
        module = load_module("device/recovery/tcp_control.py")
        logger = FakeLogger()
        transport = module.TCPControlTransport(lambda: {"server": {"host": "127.0.0.1", "tcp_port": 1}}, "ABC", logger)
        sent = []
        transport.sock = type("Sock", (), {"send": lambda _self, data: sent.append(bytes(data)) or len(data)})()
        transport.sock_key = ("127.0.0.1", 1, "ABC")
        transport.hello_sent = True

        self.assertTrue(transport.publish_status({"message": "status_announce", "seq": 1}, True))
        self.assertTrue(transport.publish_status({"message": "status_announce", "seq": 2}, True))
        self.assertTrue(transport.publish_result({"message": "important", "request_id": "r-1"}, True))

        self.assertEqual(2, transport.outbox_size())
        while transport.outbox_size():
            self.assertTrue(transport.flush())
        joined = b"".join(sent)
        self.assertNotIn(b'"seq":1', joined)
        self.assertIn(b'"seq":2', joined)
        self.assertIn(b'"request_id":"r-1"', joined)

    def test_result_preempts_status_and_progress_when_outbox_is_full(self):
        module = load_module("device/recovery/tcp_control.py")
        logger = FakeLogger()
        transport = module.TCPControlTransport(lambda: {"server": {"host": "127.0.0.1", "tcp_port": 1}}, "ABC", logger)
        sent = []
        transport.sock = type("Sock", (), {"send": lambda _self, data: sent.append(bytes(data)) or len(data)})()
        transport.sock_key = ("127.0.0.1", 1, "ABC")
        transport.hello_sent = True

        for seq in range(transport.MAX_OUTBOX):
            if seq % 2:
                self.assertTrue(transport.publish_update_progress({"message": "progress", "seq": seq}, True))
            else:
                self.assertTrue(transport.publish_status({"message": "status", "seq": seq}, True))

        self.assertEqual(transport.MAX_OUTBOX, transport.outbox_size())
        self.assertTrue(transport.publish_result({"message": "important", "request_id": "r-critical"}, True))
        self.assertEqual(transport.MAX_OUTBOX, transport.outbox_size())

        self.assertTrue(transport.flush(4096))
        self.assertIn(b'"type":"result"', sent[0])
        self.assertIn(b'"request_id":"r-critical"', sent[0])

    def test_failed_connect_uses_exponential_backoff_and_reports_gateway_lost(self):
        class FailingSocket:
            def settimeout(self, _timeout):
                pass

            def connect(self, _addr):
                attempts.append(now_ms[0])
                raise OSError(116, "ETIMEDOUT")

            def close(self):
                pass

        class FakeSocketModule:
            def getaddrinfo(self, host, port):
                return [(None, None, None, None, (host, port))]

            def socket(self):
                return FailingSocket()

        for relative_path in ("device/recovery/tcp_control.py",):
            with self.subTest(path=relative_path):
                module = load_module(relative_path)
                original_socket = module.socket
                attempts = []
                now_ms = [0]
                module.socket = FakeSocketModule()
                logger = FakeLogger()
                transport = module.TCPControlTransport(lambda: {"server": {"host": "192.168.1.153", "tcp_port": 22345}}, "ABC", logger)
                transport._ticks_ms = lambda: now_ms[0]
                base_backoff = int(transport.RECONNECT_BACKOFF_MS)
                try:
                    self.assertFalse(transport.ensure_connected(True))
                    self.assertEqual(attempts, [0])
                    self.assertEqual(transport.findme_status()["state"], "gateway_lost")
                    self.assertIn("connect_failed", transport.findme_status()["last_error"])

                    now_ms[0] = base_backoff - 1
                    self.assertFalse(transport.ensure_connected(True))
                    self.assertEqual(attempts, [0])

                    now_ms[0] = base_backoff
                    self.assertFalse(transport.ensure_connected(True))
                    self.assertEqual(attempts, [0, base_backoff])

                    now_ms[0] = base_backoff * 2 - 1
                    self.assertFalse(transport.ensure_connected(True))
                    self.assertEqual(attempts, [0, base_backoff])

                    now_ms[0] = base_backoff * 3
                    self.assertFalse(transport.ensure_connected(True))
                    self.assertEqual(attempts, [0, base_backoff, base_backoff * 3])
                    self.assertLessEqual(len(logger.warns), 3)
                finally:
                    module.socket = original_socket


if __name__ == "__main__":
    unittest.main()
