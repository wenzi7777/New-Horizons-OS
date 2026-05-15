# utils.py
import time


class RateCounter:
    def __init__(self, interval_ms=1000):
        self.interval_ms = interval_ms
        self.count = 0
        self.last_ms = time.ticks_ms()
        self.rate = 0

    def tick(self):
        self.count += 1
        now = time.ticks_ms()

        if time.ticks_diff(now, self.last_ms) >= self.interval_ms:
            self.rate = self.count
            self.count = 0
            self.last_ms = now
            return self.rate

        return None


def clamp(value, low, high):
    if value < low:
        return low
    if value > high:
        return high
    return value