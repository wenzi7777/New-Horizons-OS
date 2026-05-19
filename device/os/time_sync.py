import socket
import struct
import time


class TimeSync:
    NTP_DELTA = 2208988800

    def __init__(self, servers=None, timeout=2):
        self.servers = list(servers or [])
        self.timeout = timeout
        self.synced_epoch = None
        self.synced_ticks_ms = None
        self.last_error = ""

    def sync(self):
        for server in self.servers:
            try:
                epoch = self._query_server(server)
                self.synced_epoch = epoch
                self.synced_ticks_ms = time.ticks_ms()
                self.last_error = ""
                return True
            except Exception as exc:
                self.last_error = str(exc)
        return False

    def now_epoch(self):
        if self.synced_epoch is None or self.synced_ticks_ms is None:
            return None
        delta_ms = time.ticks_diff(time.ticks_ms(), self.synced_ticks_ms)
        return self.synced_epoch + int(delta_ms // 1000)

    def millis_offset(self):
        if self.synced_ticks_ms is None:
            return None
        return int(time.ticks_diff(time.ticks_ms(), self.synced_ticks_ms) % 1000)

    def status(self):
        return {
            "synced": self.synced_epoch is not None,
            "epoch": self.now_epoch(),
            "millis": self.millis_offset(),
            "last_error": self.last_error,
        }

    def _query_server(self, host):
        packet = bytearray(48)
        packet[0] = 0x1B
        addr = socket.getaddrinfo(host, 123)[0][-1]
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.settimeout(self.timeout)
            sock.sendto(packet, addr)
            data = sock.recv(48)
        finally:
            sock.close()
        if len(data) < 48:
            raise OSError("short_ntp_reply")
        seconds = struct.unpack("!I", data[40:44])[0]
        return int(seconds - self.NTP_DELTA)
