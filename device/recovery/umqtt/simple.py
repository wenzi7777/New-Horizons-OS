import socket
import struct


class MQTTException(Exception):
    pass


class MQTTClient:
    def __init__(
        self,
        client_id,
        server,
        port=0,
        user=None,
        password=None,
        keepalive=0,
        ssl=None,
        ssl_params={},
    ):
        if port == 0:
            port = 8883 if ssl else 1883
        self.client_id = client_id
        self.sock = None
        self.server = server
        self.port = port
        self.ssl = ssl
        self.ssl_params = ssl_params
        self.pid = 0
        self.cb = None
        self.user = user
        self.pswd = password
        self.keepalive = keepalive

    def _send_str(self, s):
        self.sock.write(struct.pack("!H", len(s)))
        self.sock.write(s)

    def _read_exact(self, n):
        data = self.sock.read(n)
        if data is None or len(data) != n:
            raise OSError(-1)
        return data

    def _skip_bytes(self, n):
        while n > 0:
            chunk = self.sock.read(min(n, 256))
            if chunk is None or len(chunk) == 0:
                raise OSError(-1)
            n -= len(chunk)

    def _recv_len(self):
        n = 0
        sh = 0
        while 1:
            b = self._read_exact(1)[0]
            n |= (b & 0x7F) << sh
            if not b & 0x80:
                return n
            sh += 7
            if sh > 21:
                raise MQTTException("malformed remaining length")

    def set_callback(self, f):
        self.cb = f

    def connect(self, clean_session=True, timeout=None):
        self.sock = socket.socket()
        self.sock.settimeout(timeout)
        addr = socket.getaddrinfo(self.server, self.port)[0][-1]
        self.sock.connect(addr)
        if self.ssl is True:
            import ssl

            self.sock = ssl.wrap_socket(self.sock, **self.ssl_params)
        elif self.ssl:
            self.sock = self.ssl.wrap_socket(self.sock, server_hostname=self.server)
        premsg = bytearray(b"\x10\0\0\0\0\0")
        msg = bytearray(b"\x04MQTT\x04\x02\0\0")
        sz = 10 + 2 + len(self.client_id)
        msg[6] = clean_session << 1
        if self.user:
            sz += 2 + len(self.user) + 2 + len(self.pswd)
            msg[6] |= 0xC0
        if self.keepalive:
            msg[7] |= self.keepalive >> 8
            msg[8] |= self.keepalive & 0x00FF
        i = 1
        while sz > 0x7F:
            premsg[i] = (sz & 0x7F) | 0x80
            sz >>= 7
            i += 1
        premsg[i] = sz
        self.sock.write(premsg, i + 2)
        self.sock.write(msg)
        self._send_str(self.client_id)
        if self.user:
            self._send_str(self.user)
            self._send_str(self.pswd)
        resp = self.sock.read(4)
        if resp[3] != 0:
            raise MQTTException(resp[3])
        return resp[2] & 1

    def disconnect(self):
        if self.sock is None:
            return
        self.sock.write(b"\xe0\0")
        self.sock.close()

    def publish(self, topic, msg, retain=False, qos=0):
        pkt = bytearray(b"\x30\0\0\0")
        pkt[0] |= qos << 1 | retain
        sz = 2 + len(topic) + len(msg)
        if qos > 0:
            sz += 2
        i = 1
        while sz > 0x7F:
            pkt[i] = (sz & 0x7F) | 0x80
            sz >>= 7
            i += 1
        pkt[i] = sz
        self.sock.write(pkt, i + 1)
        self._send_str(topic)
        if qos > 0:
            self.pid += 1
            pid = self.pid
            struct.pack_into("!H", pkt, 0, pid)
            self.sock.write(pkt, 2)
        self.sock.write(msg)

    def subscribe(self, topic, qos=0):
        assert self.cb is not None
        pkt = bytearray(b"\x82\0\0\0")
        self.pid += 1
        struct.pack_into("!BH", pkt, 1, 2 + 2 + len(topic) + 1, self.pid)
        self.sock.write(pkt)
        self._send_str(topic)
        self.sock.write(qos.to_bytes(1, "little"))
        while 1:
            res = self.sock.read(1)
            if res is None:
                continue
            if res == b"":
                raise OSError(-1)
            op = res[0]
            sz = self._recv_len()
            if op == 0x90:
                resp = self._read_exact(sz)
                if len(resp) < 3 or resp[-1] == 0x80:
                    raise MQTTException(resp[-1] if resp else 0x80)
                return
            if op & 0xF0 == 0x30:
                self._handle_publish(op, sz)
            else:
                self._skip_bytes(sz)

    def _handle_publish(self, op, sz):
        topic_len = self._read_exact(2)
        topic_len = (topic_len[0] << 8) | topic_len[1]
        topic = self._read_exact(topic_len)
        sz -= topic_len + 2
        if op & 6:
            pid = self._read_exact(2)
            pid = pid[0] << 8 | pid[1]
            sz -= 2
        msg = self._read_exact(sz)
        self.cb(topic, msg)
        if (op & 6) == 2:
            pkt = bytearray(b"\x40\x02\0\0")
            struct.pack_into("!H", pkt, 2, pid)
            self.sock.write(pkt)

    def wait_msg(self):
        res = self.sock.read(1)
        self.sock.setblocking(True)
        if res is None:
            return None
        if res == b"":
            raise OSError(-1)
        op = res[0]
        sz = self._recv_len()
        if op & 0xF0 != 0x30:
            self._skip_bytes(sz)
            return op
        self._handle_publish(op, sz)
        return op

    def check_msg(self):
        self.sock.setblocking(False)
        return self.wait_msg()
