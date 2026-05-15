import json
import socket


class UDPControlServer:
    def __init__(self, bind_port, logger=None):
        self.bind_port = bind_port
        self.logger = logger
        self.sock = None

    def begin(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", self.bind_port))
        try:
            self.sock.setblocking(False)
        except Exception:
            self.sock.settimeout(0)

    def poll(self, handler):
        if self.sock is None:
            return
        while True:
            try:
                payload, addr = self.sock.recvfrom(2048)
            except OSError:
                return

            try:
                request = json.loads(payload.decode())
            except Exception:
                self._reply(addr, {
                    "status": "error",
                    "message": "invalid_json",
                    "error": "decode_failed",
                    "reboot_required": False,
                })
                continue

            response = handler(request, addr)
            if response is None:
                continue
            self._reply(addr, response)

    def _reply(self, addr, response):
        try:
            self.sock.sendto(json.dumps(response).encode(), addr)
        except Exception as exc:
            if self.logger:
                self.logger.warn("udp_reply_failed {}".format(exc))
