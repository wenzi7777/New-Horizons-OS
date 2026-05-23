import socket
import time

import nhcp


class UDPControlTransport:
    protocol = nhcp.PROTOCOL
    RECV_BYTES = 1400
    RESULT_REPEATS = 3
    WOULD_BLOCK_ERRNOS = (11, 115)

    def __init__(self, runtime_getter, device_uid, logger=None, hello_getter=None, findme_handler=None):
        self.runtime_getter = runtime_getter
        self.device_uid = device_uid
        self.logger = logger
        self.hello_getter = hello_getter
        self.findme_handler = findme_handler
        self.sock = None
        self.key = None
        self.addr = None
        self.pending = []
        self.recent_results = {}
        self.seq = 0
        self.last_seen_seq = 0
        self.hello_sent = False
        self.findme_state = "idle"
        self.findme_gateway_id = ""
        self.findme_session_id = ""
        self.findme_last_error = ""

    def poll(self, wifi_connected, handler=None):
        if not self.ensure_connected(wifi_connected):
            return False
        if not self.hello_sent:
            self._send_hello()
        self._read_available()
        if handler is not None:
            while self.pending:
                request = self.pending.pop(0)
                command = self._command_name(request)
                self._info("udp_command_received command={}".format(command))
                try:
                    response = handler(request, ("udp", 0))
                    self._info(
                        "udp_command_done command={} status={} message={}".format(
                            command,
                            self._response_value(response, "status"),
                            self._response_value(response, "message"),
                        )
                    )
                except Exception as exc:
                    self._warn("udp_command_failed command={} error={}".format(command, exc))
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
        return self._send_frame("status", payload or {}, repeats=1)

    def publish_result(self, payload, wifi_connected):
        if not self.ensure_connected(wifi_connected):
            return False
        request_id = ""
        if isinstance(payload, dict):
            request_id = str(payload.get("request_id") or "")
        if request_id:
            self.recent_results[request_id] = dict(payload)
            self._trim_recent_results()
        return self._send_frame("result", payload or {}, repeats=self.RESULT_REPEATS)

    def publish_update_progress(self, payload, wifi_connected):
        if not self.ensure_connected(wifi_connected):
            return False
        return self._send_frame("update_progress", payload or {}, repeats=self.RESULT_REPEATS)

    def is_connected(self):
        return self.sock is not None and self.addr is not None

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except Exception:
                pass
        self.sock = None
        self.key = None
        self.addr = None
        self.hello_sent = False

    def reconfigure(self):
        self.close()
        self.findme_state = "idle"
        self.findme_last_error = ""

    def findme_status(self):
        return {
            "state": self.findme_state,
            "gateway_id": self.findme_gateway_id,
            "session_id": self.findme_session_id,
            "last_error": self.findme_last_error,
            "connected": self.is_connected(),
        }

    def ensure_connected(self, wifi_connected):
        if not wifi_connected:
            self.close()
            return False
        runtime = self.runtime_getter()
        server_cfg = runtime.get("server", {})
        host = server_cfg.get("host", "")
        port = int(server_cfg.get("udp_port", 13250))
        gateway_id = str(server_cfg.get("gateway_id") or "")
        if not host:
            return False
        key = (host, port, self.device_uid)
        if self.sock is not None and self.key == key:
            return True
        self.close()
        try:
            self.addr = socket.getaddrinfo(host, port)[0][-1]
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.settimeout(0)
            self.key = key
            self.findme_state = "attached"
            self.findme_gateway_id = gateway_id
            self.findme_last_error = ""
            self._send_hello()
            return True
        except Exception as exc:
            self.findme_state = "gateway_lost"
            self.findme_last_error = "udp_connect_failed:{}".format(exc)
            self._warn("udp_control_open_failed {}".format(exc))
            self.close()
            return False

    def _send_hello(self):
        payload = {}
        if self.hello_getter is not None:
            try:
                payload = self.hello_getter() or {}
            except Exception as exc:
                payload = {"status": "error", "message": "hello_failed", "error": str(exc)}
        ok = self._send_frame("hello", payload, repeats=1)
        self.hello_sent = bool(ok)
        return ok

    def _read_available(self):
        while self.sock is not None:
            try:
                data, _addr = self.sock.recvfrom(self.RECV_BYTES)
            except Exception as exc:
                if not self._is_would_block(exc):
                    self.findme_last_error = "recv_failed:{}".format(exc)
                    self._warn("udp_control_recv_failed {}".format(exc))
                    self.close()
                return
            if not data:
                return
            try:
                frame = nhcp.decode_frame(data)
            except Exception as exc:
                self._warn("udp_control_decode_failed {}".format(exc))
                continue
            self.last_seen_seq = int(frame.get("seq", 0) or 0)
            if frame.get("type") == "ack":
                continue
            if frame.get("type") == "command":
                payload = frame.get("payload", {})
                if not isinstance(payload, dict):
                    continue
                request_id = str(payload.get("request_id") or "")
                if request_id and request_id in self.recent_results:
                    self.publish_result(self.recent_results[request_id], True)
                    continue
                if len(self.pending) >= 8:
                    self.pending.pop(0)
                self.pending.append(dict(payload))
                self._send_ack(frame)

    def _send_ack(self, frame):
        payload = {"request_id": (frame.get("payload") or {}).get("request_id", "")}
        return self._send_frame("ack", payload, repeats=1, ack=int(frame.get("seq", 0) or 0))

    def _send_frame(self, msg_type, payload, repeats=1, ack=0):
        if self.sock is None or self.addr is None:
            return False
        ok = False
        repeats = max(1, int(repeats or 1))
        for _idx in range(repeats):
            self.seq = (int(self.seq) + 1) & 0xffff
            try:
                packet = nhcp.encode_frame(
                    msg_type,
                    device_uid=self.device_uid,
                    payload=payload or {},
                    seq=self.seq,
                    ack=ack or self.last_seen_seq,
                )
                self.sock.sendto(packet, self.addr)
                ok = True
            except Exception as exc:
                self.findme_last_error = "send_failed:{}".format(exc)
                self._warn("udp_control_send_failed {}".format(exc))
                self.close()
                return False
        return ok

    def _trim_recent_results(self):
        if len(self.recent_results) <= 8:
            return
        for key in list(self.recent_results.keys())[:-8]:
            self.recent_results.pop(key, None)

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

    def _is_would_block(self, exc):
        try:
            code = exc.args[0]
        except Exception:
            code = None
        return code in self.WOULD_BLOCK_ERRNOS

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
