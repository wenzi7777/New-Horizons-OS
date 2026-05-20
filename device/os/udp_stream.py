import socket


class UDPStreamTransport:
    def __init__(self, runtime_getter, logger=None):
        self.runtime_getter = runtime_getter
        self.logger = logger
        self.sock = None
        self.key = None
        self.addr = None

    def send(self, payload, wifi_connected):
        if not wifi_connected:
            self.close()
            return False
        if not self._ensure_socket():
            return False
        try:
            self.sock.sendto(payload, self.addr)
            return True
        except Exception as exc:
            self._warn("udp_stream_send_failed {}".format(exc))
            self.close()
            return False

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except Exception:
                pass
        self.sock = None
        self.key = None
        self.addr = None

    def reconfigure(self):
        self.close()

    def _ensure_socket(self):
        runtime = self.runtime_getter()
        server_cfg = runtime.get("server", {})
        host = server_cfg.get("host", "")
        port = int(server_cfg.get("udp_port", 13250))
        if not host:
            return False
        key = (host, port)
        if self.sock is not None and self.key == key:
            return True
        self.close()
        try:
            self.addr = socket.getaddrinfo(host, port)[0][-1]
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.key = key
            return True
        except Exception as exc:
            self._warn("udp_stream_open_failed {}".format(exc))
            self.close()
            return False

    def _warn(self, message):
        if self.logger:
            self.logger.warn(message)
        else:
            print(message)
