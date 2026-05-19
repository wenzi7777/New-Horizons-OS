import time

from . import simple


class MQTTClient(simple.MQTTClient):
    DELAY = 2

    def delay(self, _index):
        time.sleep(self.DELAY)

    def reconnect(self):
        index = 0
        while 1:
            try:
                return super().connect(False)
            except OSError:
                index += 1
                self.delay(index)

    def publish(self, topic, msg, retain=False, qos=0):
        while 1:
            try:
                return super().publish(topic, msg, retain, qos)
            except OSError:
                self.reconnect()

    def wait_msg(self):
        while 1:
            try:
                return super().wait_msg()
            except OSError:
                self.reconnect()

    def check_msg(self, attempts=2):
        while attempts:
            self.sock.setblocking(False)
            try:
                return super().wait_msg()
            except OSError:
                self.reconnect()
                attempts -= 1
        return None
