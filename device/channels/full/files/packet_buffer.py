# packet_buffer.py

class PacketBuffer:
    def __init__(self, capacity=3, drop_oldest=True):
        if capacity < 1:
            capacity = 1

        self.capacity = capacity
        self.drop_oldest = drop_oldest

        self.buf = [None] * capacity
        self.head = 0
        self.tail = 0
        self.count = 0

        self.dropped = 0
        self.pushed = 0
        self.popped = 0

    def push(self, packet):
        if self.count >= self.capacity:
            if self.drop_oldest:
                # Drop oldest packet.
                self.buf[self.tail] = None
                self.tail = (self.tail + 1) % self.capacity
                self.count -= 1
                self.dropped += 1
            else:
                # Drop new packet.
                self.dropped += 1
                return False

        self.buf[self.head] = packet
        self.head = (self.head + 1) % self.capacity
        self.count += 1
        self.pushed += 1
        return True

    def pop(self):
        if self.count <= 0:
            return None

        packet = self.buf[self.tail]
        self.buf[self.tail] = None

        self.tail = (self.tail + 1) % self.capacity
        self.count -= 1
        self.popped += 1

        return packet

    def clear(self):
        for i in range(self.capacity):
            self.buf[i] = None

        self.head = 0
        self.tail = 0
        self.count = 0

    def available(self):
        return self.count

    def stats(self):
        return {
            "count": self.count,
            "capacity": self.capacity,
            "pushed": self.pushed,
            "popped": self.popped,
            "dropped": self.dropped,
        }