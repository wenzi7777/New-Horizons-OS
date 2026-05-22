import json
import socket
import time


class TCPControlTransport:
    MAX_PENDING = 8
    MAX_OUTBOX = 8
    FLUSH_BYTES_PER_POLL = 1024
    RECONNECT_BACKOFF_MS = 5000
    RECONNECT_BACKOFF_MAX_MS = 30000
    CONNECT_TIMEOUT_SEC = 0.5
    RECV_CHUNK = 512
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
        self.outbox = []
        self.last_attempt_ms = -self.RECONNECT_BACKOFF_MS
        self.connect_failures = 0
        self.hello_sent = False
        self.findme_state = "idle"
        self.findme_gateway_id = ""
        self.findme_session_id = ""
        self.findme_last_error = ""

    def poll(self, wifi_connected, handler=None):
        if not self.ensure_connected(wifi_connected):
            return False
        self.flush()
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
        self.flush()
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
        self.outbox = []
        self.hello_sent = False
        if self.findme_state == "attached":
            self.findme_state = "gateway_lost"

    def reconfigure(self):
        self.last_attempt_ms = -self.RECONNECT_BACKOFF_MS
        self.connect_failures = 0
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
        if self._ticks_diff(now, self.last_attempt_ms) < self._connect_backoff_ms():
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
            self.connect_failures = 0
            self._info("findme_attach_start host={} port={}".format(host, port))
            return self._send_hello()
        except Exception as exc:
            self.connect_failures += 1
            self._mark_gateway_lost("connect_failed:{}".format(exc))
            self._warn("tcp_control_connect_failed {} next_retry_ms={}".format(exc, self._connect_backoff_ms()))
            self.close()
            return False

    def _connect_backoff_ms(self):
        base = int(self.RECONNECT_BACKOFF_MS)
        if self.connect_failures <= 0:
            return base
        shift = min(int(self.connect_failures) - 1, 6)
        delay = base * (1 << shift)
        return min(delay, int(self.RECONNECT_BACKOFF_MAX_MS))

    def _mark_gateway_lost(self, error):
        if self.findme_state != "rejected":
            self.findme_state = "gateway_lost"
        self.findme_last_error = str(error or "gateway_lost")

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
        try:
            data = (json.dumps(body, separators=(",", ":")) + "\n").encode()
        except TypeError:
            data = (json.dumps(body) + "\n").encode()
        return self._enqueue_bytes(message_type, payload or {}, data)

    def _enqueue_bytes(self, message_type, payload, data):
        if self.sock is None:
            return False
        item = {
            "type": message_type,
            "message": str(payload.get("message") or "") if isinstance(payload, dict) else "",
            "data": data,
            "offset": 0,
        }
        if self._is_coalescable_status(item):
            self.outbox = [queued for queued in self.outbox if not self._is_coalescable_status(queued)]
        if message_type == "hello":
            self.outbox.insert(0, item)
            return True
        if len(self.outbox) >= self.MAX_OUTBOX:
            dropped = False
            for idx, queued in enumerate(self.outbox):
                if queued.get("type") in ("status", "update_progress"):
                    self.outbox.pop(idx)
                    dropped = True
                    break
            if not dropped:
                if message_type == "result":
                    self.findme_last_error = "outbox_full"
                    self._warn("tcp_control_outbox_full result_preserved_failed")
                return False
        self.outbox.append(item)
        return True

    def _is_coalescable_status(self, item):
        return item.get("type") == "status" and item.get("message") == "status_announce"

    def flush(self, max_bytes=None):
        return self._flush_outbox(max_bytes)

    def outbox_size(self):
        return len(self.outbox)

    def _flush_outbox(self, max_bytes=None):
        if self.sock is None:
            return False
        remaining = int(max_bytes or self.FLUSH_BYTES_PER_POLL)
        while self.outbox and remaining > 0:
            item = self.outbox[0]
            data = item.get("data", b"")
            offset = int(item.get("offset") or 0)
            if offset >= len(data):
                self.outbox.pop(0)
                continue
            try:
                written = self.sock.send(data[offset: offset + remaining])
            except Exception as exc:
                if self._is_would_block(exc):
                    self.findme_last_error = "send_backpressure"
                    return False
                self.findme_last_error = "send_failed:{}".format(exc)
                self._warn("tcp_control_send_failed {}".format(exc))
                self.close()
                return False
            if written is None:
                written = min(remaining, len(data) - offset)
            written = int(written)
            if written <= 0:
                self.findme_last_error = "send_backpressure"
                return False
            item["offset"] = offset + written
            remaining -= written
            if int(item.get("offset") or 0) >= len(data):
                self.outbox.pop(0)
        return not self.outbox

    def _send_bytes(self, data):
        return self._enqueue_bytes("raw", {}, data)

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
