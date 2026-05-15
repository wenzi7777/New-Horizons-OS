try:
    import time
except ImportError:  # pragma: no cover
    time = None

try:
    import machine
except ImportError:  # pragma: no cover
    machine = None

import config
from frame_protocol import encode_scan_frame, decode_scan_frame


def _ticks_ms():
    if time is None:  # pragma: no cover
        return 0
    if hasattr(time, "ticks_ms"):
        return time.ticks_ms()
    return int(time.time() * 1000) & 0xFFFFFFFF


def _ticks_diff(now, then):
    if time is None:  # pragma: no cover
        return 0
    if hasattr(time, "ticks_diff"):
        return time.ticks_diff(now, then)
    return now - then


class _RingBuffer:
    def __init__(self, capacity):
        self.capacity = max(1, int(capacity))
        self.frames = [None] * self.capacity
        self.read_index = 0
        self.write_index = 0
        self.count = 0
        self.produced = 0
        self.consumed = 0
        self.dropped = 0
        self.last_written_seq = -1
        self.last_read_seq = -1

    def push(self, frame_bytes, seq):
        if self.count == self.capacity:
            self.read_index = (self.read_index + 1) % self.capacity
            self.count -= 1
            self.dropped += 1

        self.frames[self.write_index] = frame_bytes
        self.write_index = (self.write_index + 1) % self.capacity
        self.count += 1
        self.produced += 1
        self.last_written_seq = int(seq)

    def pop(self):
        if self.count == 0:
            return None
        frame = self.frames[self.read_index]
        self.frames[self.read_index] = None
        self.read_index = (self.read_index + 1) % self.capacity
        self.count -= 1
        self.consumed += 1
        if frame is not None:
            self.last_read_seq = decode_scan_frame(frame)["seq"]
        return frame

    def peek_latest(self):
        if self.count == 0:
            return None
        latest_index = (self.write_index - 1) % self.capacity
        return self.frames[latest_index]

    def stats(self):
        return {
            "capacity": self.capacity,
            "available": self.count,
            "produced_frames": self.produced,
            "consumed_frames": self.consumed,
            "dropped_frames": self.dropped,
            "last_written_seq": self.last_written_seq,
            "last_read_seq": self.last_read_seq,
        }


class _ScanAPI:
    def __init__(self):
        self.scanner = None
        self.rows = config.ROWS
        self.cols = config.COLS
        self.row_pins = list(config.ACTIVE_ROWS)
        self.col_pins = list(config.ACTIVE_COLS)
        self.fps = config.TARGET_FPS
        self.settle_us = config.MATRIX_SETTLE_US
        self.buffer_frames = 8
        self.core_id = 1
        self.seq = 0
        self.started = False
        self.last_capture_ms = 0
        self.buffer = _RingBuffer(self.buffer_frames)

    def init(self, rows=None, cols=None, row_pins=None, col_pins=None, *, layout=None, fps=None, settle_us=None, buffer_frames=8, core_id=1):
        layout = layout or {}
        if rows is None:
            rows = layout.get("rows", len(config.ACTIVE_ROWS))
        if cols is None:
            cols = layout.get("cols", len(config.ACTIVE_COLS))
        if row_pins is None:
            row_pins = layout.get("row_pins", config.ACTIVE_ROWS)
        if col_pins is None:
            col_pins = layout.get("col_pins", config.ACTIVE_COLS)

        self.row_pins = list(row_pins)
        self.col_pins = list(col_pins)
        self.rows = int(rows if rows is not None else len(self.row_pins))
        self.cols = int(cols if cols is not None else len(self.col_pins))
        if self.rows != len(self.row_pins):
            self.rows = len(self.row_pins)
        if self.cols != len(self.col_pins):
            self.cols = len(self.col_pins)
        self.fps = int(fps if fps is not None else config.TARGET_FPS)
        self.settle_us = int(settle_us if settle_us is not None else config.MATRIX_SETTLE_US)
        self.buffer_frames = int(buffer_frames)
        self.core_id = int(core_id)
        self.buffer = _RingBuffer(self.buffer_frames)
        self.seq = 0
        self.last_capture_ms = 0

        scanner_impl = _load_scanner_impl()
        self.scanner = scanner_impl(
            rows=self.rows,
            cols=self.cols,
            active_count=len(self.row_pins) * len(self.col_pins)
        )
        return True

    def set_layout(self, rows, cols, row_pins, col_pins):
        return self.init(
            layout={
                "rows": rows,
                "cols": cols,
                "row_pins": list(row_pins),
                "col_pins": list(col_pins),
            },
            fps=self.fps,
            settle_us=self.settle_us,
            buffer_frames=self.buffer_frames,
            core_id=self.core_id,
        )

    def start(self):
        if self.scanner is None:
            self.init()
        self.scanner.begin()
        self.started = True
        return True

    def stop(self):
        self.started = False
        return True

    def service(self):
        if not self.started or time is None:
            return False
        now = _ticks_ms()
        interval = int(1000 / max(1, self.fps))
        if _ticks_diff(now, self.last_capture_ms) < interval:
            return False
        self.last_capture_ms = now
        matrix = self.scanner.scan_once()
        payload = [int(round(v)) for v in matrix]
        frame = encode_scan_frame(self.seq, now, self.rows, self.cols, payload)
        self.buffer.push(frame, self.seq)
        self.seq += 1
        return True

    def pop_frame_mv(self):
        frame = self.buffer.pop()
        if frame is None:
            return None
        return memoryview(frame)

    def peek_latest_mv(self):
        frame = self.buffer.peek_latest()
        if frame is None:
            return None
        return memoryview(frame)

    def sample_cell_mv(self, analog_pin, select_pin, duration_ms):
        if self.scanner is None:
            self.init()
            self.scanner.begin()
        return self.scanner.sample_cell(analog_pin, select_pin, duration_ms)

    def stats(self):
        stats = self.buffer.stats()
        return (
            stats["produced_frames"],
            stats["consumed_frames"],
            stats["dropped_frames"],
            stats["last_written_seq"],
            stats["last_read_seq"],
            self.buffer_frames,
            self.core_id,
            1 if self.started else 0,
            self.rows,
            self.cols,
        )


class _SysAPI:
    def reboot(self):
        if machine is None:  # pragma: no cover
            raise RuntimeError("machine unavailable")
        machine.reset()

    def info(self):
        return {
            "module": "vdboard-shim",
            "native_scan": False,
            "native_wifi_setup": False,
            "frame_header_size": 16,
        }


scan = _ScanAPI()
sys = _SysAPI()


def _load_scanner_impl():
    if config.USE_MATRIX_MOCK or machine is None:
        from matrix_scan_mock import MatrixScanner as scanner_impl
        return scanner_impl

    from matrix_scan import MatrixScanner as scanner_impl
    return scanner_impl
