# udp_stream.py
import socket
import time
import gc
import config


class UDPStreamer:
    def __init__(self, host, port):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        self.fail_count = 0
        self.last_error_ms = 0

        print("UDP target:", self.addr)

    def send(self, data):
        try:
            self.sock.sendto(data, self.addr)
            self.fail_count = 0
            return True

        except OSError as e:
            self.fail_count += 1

            now = time.ticks_ms()
            if time.ticks_diff(now, self.last_error_ms) > 1000:
                self.last_error_ms = now

                if config.PRINT_PACKET_ERROR:
                    print("UDP send failed:", e, "fail_count:", self.fail_count)

            if self.fail_count % 10 == 0:
                gc.collect()

            return False