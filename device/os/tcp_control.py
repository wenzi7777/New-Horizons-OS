import json
import socket
import time


class TCPControlTransport:
    MAX_PENDING = 8
    RECONNECT_BACKOFF_MS = 2000
    CONNECT_TIMEOUT_SEC = 1
    RECV_CHUNK = 512
    SEND_TIMEOUT_MS = 300
    SEND_RETRY_MS = 10
    WOULD_BLOCK_ERRNOS = (11, 115)

    def __init__(self, runtime_getter, device_uid, logger=None, hello_getter=None, findme_handler=None):
        self.runtime_getter = runtime_getter
        self.device_uid = device_uid
        self.logger = logger
        self.hello_getter = hello_getter
        self.findme_handler = findme_handler
        self.sock = None
        self.sock_key = None
        self.rx = b""
        self.pending = []
        self.last_attempt_ms = 0
        self.hello_sent = False
        self.findme_state = "idle"
        self.findme_gateway_id = ""
        self.findme_session_id = ""
        self.findme_last_error = ""

    def poll(self, wifi_connected, handler=None):
        if not self.ensure_connected(wifi_connected):
            return False
        self._read_available()
        if handler is not None:
            while self.pending:
                request = self.pending.pop(0)
                command = self._command_name(request)
                self._info("tcp_command_received command={}".format(command))
                try:
                    response = handler(request, ("tcp", 0))
                    self._info(
                        "tcp_command_done command={} status={} message={}".format(
                            command,
                            self._response_value(response, "status"),
                            self._response_value(response, "message"),
                        )
                    )
                except Exception as exc:
                    self._warn("tcp_command_failed command={} error={}".format(command, exc))
                    response = {
                        "status": "error",
                        "message": "command_failed",
                        "command": command,
                        "error": str(exc),
                        "reboot_required": False,
                    }
                if response is not None:
                    self.publish_result(self._annotate_response(request, response, command), wifi_connected)
        return True

    def publish_status(self, payload, wifi_connected):
        if not self.ensure_connected(wifi_connected):
            return False
        return self._send_json("status", payload)

    def publish_result(self, payload, wifi_connected):
        if not self.ensure_connected(wifi_connected):
            return False
        return self._send_json("result", payload)

    def publish_update_progress(self, payload, wifi_connected):
        if not self.ensure_connected(wifi_connected):
            return False
        return self._send_json("update_progress", payload)

    def is_connected(self):
        return self.sock is not None

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except Exception:
                pass
        self.sock = None
        self.sock_key = None
        self.rx = b""
        self.hello_sent = False
        if self.findme_state == "attached":
            self.findme_state = "gateway_lost"

    def reconfigure(self):
        self.last_attempt_ms = 0
        self.close()

    def ensure_connected(self, wifi_connected):
        if not wifi_connected:
            self.close()
            return False
        runtime = self.runtime_getter()
        server_cfg = runtime.get("server", {})
        host = server_cfg.get("host", "")
        port = int(server_cfg.get("tcp_port", 22345))
        if not host:
            return False
        key = (host, port, self.device_uid)
        if self.sock is not None and self.sock_key == key:
            if not self.hello_sent:
                return self._send_hello()
            return True
        now = self._ticks_ms()
        if self._ticks_diff(now, self.last_attempt_ms) < self.RECONNECT_BACKOFF_MS:
            return False
        self.last_attempt_ms = now
        self.close()
        try:
            addr = socket.getaddrinfo(host, port)[0][-1]
            sock = socket.socket()
            sock.settimeout(self.CONNECT_TIMEOUT_SEC)
            sock.connect(addr)
            sock.settimeout(0)
            self.sock = sock
            self.sock_key = key
            self.findme_state = "attaching"
            self.findme_last_error = ""
            self._info("findme_attach_start host={} port={}".format(host, port))
            return self._send_hello()
        except Exception as exc:
            self._warn("tcp_control_connect_failed {}".format(exc))
            self.close()
            return False

    def _send_hello(self):
        payload = {}
        if self.hello_getter is not None:
            try:
                payload = self.hello_getter() or {}
            except Exception as exc:
                payload = {"status": "error", "message": "hello_failed", "error": str(exc)}
        ok = self._send_json("hello", payload)
        self.hello_sent = bool(ok)
        return ok

    def _send_json(self, message_type, payload):
        if self.sock is None:
            return False
        body = {
            "type": message_type,
            "device_uid": self.device_uid,
            "payload": payload or {},
        }
        data = (json.dumps(body) + "\n").encode()
        return self._send_bytes(data)

    def _send_bytes(self, data):
        if self.sock is None:
            return False
        deadline = self._ticks_add(self._ticks_ms(), self.SEND_TIMEOUT_MS)
        total = len(data)
        sent = 0
        view = memoryview(data)
        while sent < total:
            try:
                written = self.sock.send(view[sent:])
            except Exception as exc:
                if self._is_would_block(exc) and self._ticks_diff(deadline, self._ticks_ms()) > 0:
                    self._sleep_ms(self.SEND_RETRY_MS)
                    continue
                self.findme_last_error = "send_failed:{}".format(exc)
                self._warn("tcp_control_send_failed {}".format(exc))
                self.close()
                return False
            if written is None:
                return True
            if written <= 0:
                if self._ticks_diff(deadline, self._ticks_ms()) > 0:
                    self._sleep_ms(self.SEND_RETRY_MS)
                    continue
                self.findme_last_error = "send_failed:zero_write"
                self._warn("tcp_control_send_failed zero_write")
                self.close()
                return False
            sent += int(written)
        return True

    def _is_would_block(self, exc):
        try:
            code = exc.args[0]
        except Exception:
            code = None
        return code in self.WOULD_BLOCK_ERRNOS

    def _sleep_ms(self, delay_ms):
        try:
            time.sleep_ms(delay_ms)
        except AttributeError:
            time.sleep(delay_ms / 1000.0)

    def _read_available(self):
        if self.sock is None:
            return
        while True:
            try:
                data = self.sock.recv(self.RECV_CHUNK)
            except Exception as exc:
                if not self._is_would_block(exc):
                    self.findme_last_error = "recv_failed:{}".format(exc)
                    self._warn("tcp_control_recv_failed {}".format(exc))
                    self.close()
                return
            if not data:
                self.close()
                return
            self.rx += data
            while b"\n" in self.rx:
                line, self.rx = self.rx.split(b"\n", 1)
                self._on_line(line)
            if len(data) < self.RECV_CHUNK:
                return

    def _on_line(self, line):
        try:
            data = json.loads(line.decode())
        except Exception as exc:
            self._warn("tcp_control_decode_failed {}".format(exc))
            return
        if not isinstance(data, dict):
            return
        if data.get("type") == "nh_findme_accept":
            self.findme_state = "attached"
            self.findme_gateway_id = str(data.get("gateway_id") or "")
            self.findme_session_id = str(data.get("session_id") or "")
            self.findme_last_error = ""
            self._info("findme_attach_ok gateway_id={}".format(self.findme_gateway_id))
            if self.findme_handler is not None:
                try:
                    self.findme_handler(dict(data))
                except Exception as exc:
                    self._warn("findme_handler_failed {}".format(exc))
            return
        if data.get("type") == "nh_findme_reject":
            self.findme_state = "rejected"
            self.findme_gateway_id = str(data.get("gateway_id") or "")
            self.findme_last_error = str(data.get("reason") or "device_rejected")
            self._warn("findme_attach_rejected gateway_id={} reason={}".format(self.findme_gateway_id, self.findme_last_error))
            if self.findme_handler is not None:
                try:
                    self.findme_handler(dict(data))
                except Exception as exc:
                    self._warn("findme_handler_failed {}".format(exc))
            self.close()
            return
        if data.get("type") == "command" and isinstance(data.get("payload"), dict):
            request = dict(data.get("payload"))
            if data.get("request_id") and "request_id" not in request:
                request["request_id"] = data.get("request_id")
            if len(self.pending) >= self.MAX_PENDING:
                self.pending.pop(0)
            self.pending.append(request)

    def findme_status(self):
        return {
            "state": self.findme_state,
            "gateway_id": self.findme_gateway_id,
            "session_id": self.findme_session_id,
            "last_error": self.findme_last_error,
            "connected": self.is_connected(),
        }

    def _command_name(self, request):
        if not isinstance(request, dict):
            return "unknown"
        return str(request.get("command", request.get("cmd", "unknown")) or "unknown")

    def _response_value(self, response, key):
        if not isinstance(response, dict):
            return ""
        value = response.get(key, "")
        return "" if value is None else value

    def _annotate_response(self, request, response, command):
        if not isinstance(response, dict):
            return response
        if "command" not in response:
            response["command"] = command
        if isinstance(request, dict):
            request_id = request.get("request_id", "")
            if request_id and "request_id" not in response:
                response["request_id"] = request_id
        return response

    def _ticks_ms(self):
        return time.ticks_ms() if hasattr(time, "ticks_ms") else int(time.time() * 1000)

    def _ticks_add(self, now, delta):
        return time.ticks_add(now, delta) if hasattr(time, "ticks_add") else now + delta

    def _ticks_diff(self, now, then):
        return time.ticks_diff(now, then) if hasattr(time, "ticks_diff") else now - then

    def _info(self, message):
        if self.logger:
            self.logger.info(message)
        else:
            print(message)

    def _warn(self, message):
        if self.logger:
            self.logger.warn(message)
        else:
            print(message)
