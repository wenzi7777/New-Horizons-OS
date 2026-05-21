try:
    import time
except ImportError:  # pragma: no cover
    time = None

try:
    import machine
except ImportError:  # pragma: no cover
    machine = None

import struct

import config
from device_identity import get_packet_device_uid_bytes
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


def _packet_uid_bytes(value=None):
    if value is None:
        return get_packet_device_uid_bytes()
    if isinstance(value, bytes):
        data = value
    elif isinstance(value, bytearray):
        data = bytes(value)
    elif isinstance(value, str):
        text = value.replace(":", "").replace("-", "").replace(" ", "")
        try:
            data = bytes.fromhex(text)
        except Exception:
            data = value.encode()
    else:
        data = bytes(value)
    if len(data) >= 6:
        return data[-6:]
    return (b"\x00" * (6 - len(data))) + data


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
        self.device_uid = get_packet_device_uid_bytes()
        self.use_hmac = bool(getattr(config, "USE_HMAC", False))
        self.hmac_len = int(getattr(config, "HMAC_LEN", 0)) if self.use_hmac else 0
        self.hmac_key = b""
        self.filter_enabled = False
        self.filter_median = 3
        self.filter_alpha = 0.25
        self.filter_windows = []
        self.filter_state = []
        self.calibration_table = []
        self.imu_cache = None
        self.battery_cache = None
        self.packet_frames = 0

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
        self._reset_stream_state()

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

    def _reset_stream_state(self):
        sensor_count = max(0, len(self.row_pins) * len(self.col_pins))
        self.filter_windows = [[] for _ in range(sensor_count)]
        self.filter_state = [None] * sensor_count
        if len(self.calibration_table) != sensor_count:
            self.calibration_table = [[] for _ in range(sensor_count)]
        self.packet_frames = 0

    def set_packet_options(self, device_id, use_hmac, hmac_len, hmac_key):
        self.device_uid = _packet_uid_bytes(device_id)
        self.use_hmac = bool(use_hmac)
        self.hmac_len = int(hmac_len) if self.use_hmac else 0
        if isinstance(hmac_key, str):
            hmac_key = hmac_key.encode()
        self.hmac_key = bytes(hmac_key or b"")
        return True

    def configure_filter(self, enabled, median, alpha):
        median = int(median)
        if median not in (1, 3, 5):
            raise ValueError("median must be 1, 3, or 5")
        alpha = float(alpha)
        if alpha < 0.05 or alpha > 0.6:
            raise ValueError("alpha out of range")
        self.filter_enabled = bool(enabled)
        self.filter_median = median
        self.filter_alpha = alpha
        self._reset_stream_state()
        return True

    def load_calibration(self, table):
        sensor_count = max(0, len(self.row_pins) * len(self.col_pins))
        normalized = []
        table = table or []
        for idx in range(sensor_count):
            points = table[idx] if idx < len(table) and table[idx] is not None else []
            curve = [(float(sample), float(level)) for sample, level in points]
            curve.sort(key=lambda item: item[0])
            normalized.append(curve)
        self.calibration_table = normalized
        return True

    def update_imu_cache(self, values):
        if values is None or len(values) < 6:
            self.imu_cache = None
            return True
        payload = [float(values[idx]) for idx in range(6)]
        try:
            payload.append(float(values[6]))
        except Exception:
            payload.append(0.0)
        self.imu_cache = tuple(payload)
        return True

    def update_battery_cache(self, values):
        if values is None or len(values) < 3:
            self.battery_cache = None
            return True
        self.battery_cache = (
            int(values[0]) & 0xFF,
            int(values[1]) & 0xFF,
            int(values[2]) & 0xFFFF,
        )
        return True

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

    def pop_packet(self):
        frame = self.buffer.pop()
        if frame is None:
            return None
        decoded = decode_scan_frame(frame)
        matrix = [
            self._apply_calibration(idx, self._apply_filter(idx, float(value)))
            for idx, value in enumerate(decoded["payload_mv"])
        ]

        imu_payload = b""
        battery_payload = b""
        flags = 0
        if self.imu_cache is not None:
            flags |= 0x01
            imu_payload = struct.pack("<7f", *self.imu_cache)
        if self.battery_cache is not None:
            flags |= 0x02
            battery_payload = struct.pack("<BBH", *self.battery_cache)
        if self.use_hmac:
            flags |= 0x80

        matrix_payload = struct.pack("<" + ("f" * len(matrix)), *matrix) if matrix else b""
        payload = matrix_payload + imu_payload + battery_payload
        header_len = 20
        packet = bytearray(header_len + len(payload) + (self.hmac_len if self.use_hmac else 0))
        struct.pack_into(
            "<HBB",
            packet,
            0,
            int(getattr(config, "MAGIC", 0xA55A)),
            int(getattr(config, "PACKET_VERSION", 2)),
            flags,
        )
        packet[4:10] = self.device_uid
        struct.pack_into(
            "<IIH",
            packet,
            10,
            int(decoded["seq"]),
            int(decoded["timestamp_ms"]),
            len(payload),
        )
        packet[header_len:header_len + len(payload)] = payload
        if self.use_hmac and self.hmac_len:
            try:
                from crypto_hmac import hmac_sha256
                body_len = header_len + len(payload)
                packet[body_len:body_len + self.hmac_len] = hmac_sha256(self.hmac_key, packet[:body_len])[:self.hmac_len]
            except Exception:
                pass
        self.packet_frames += 1
        return bytes(packet)

    def _apply_filter(self, sensor_index, value):
        if not self.filter_enabled:
            return value
        if sensor_index < 0 or sensor_index >= len(self.filter_state):
            return value
        if self.filter_median > 1:
            window = self.filter_windows[sensor_index]
            window.append(value)
            if len(window) > self.filter_median:
                window.pop(0)
            ordered = sorted(window)
            value = ordered[len(ordered) // 2]
        previous = self.filter_state[sensor_index]
        if previous is None:
            filtered = value
        else:
            filtered = (self.filter_alpha * value) + ((1.0 - self.filter_alpha) * previous)
        self.filter_state[sensor_index] = filtered
        return filtered

    def _apply_calibration(self, sensor_index, raw_mv):
        if sensor_index < 0 or sensor_index >= len(self.calibration_table):
            return raw_mv
        curve = self.calibration_table[sensor_index]
        if len(curve) < 2:
            return raw_mv
        value = float(raw_mv)
        if value <= curve[0][0]:
            return curve[0][1]
        if value >= curve[-1][0]:
            return curve[-1][1]
        for idx in range(len(curve) - 1):
            mv0, level0 = curve[idx]
            mv1, level1 = curve[idx + 1]
            if mv0 <= value <= mv1:
                if mv1 == mv0:
                    return level1
                return level0 + ((value - mv0) / (mv1 - mv0)) * (level1 - level0)
        return value

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

    def stream_stats(self):
        stats = self.buffer.stats()
        return {
            "produced_frames": stats["produced_frames"],
            "consumed_frames": stats["consumed_frames"],
            "dropped_frames": stats["dropped_frames"],
            "packet_frames": self.packet_frames,
            "ring_count": stats["available"],
            "buffer_frames": self.buffer_frames,
            "point_count": self.rows * self.cols,
            "filter_enabled": self.filter_enabled,
            "filter_median": self.filter_median,
            "filter_alpha": self.filter_alpha,
            "imu_cached": self.imu_cache is not None,
            "battery_cached": self.battery_cache is not None,
        }

    def memory_stats(self):
        point_count = self.rows * self.cols
        return {
            "heap_free": 0,
            "heap_largest_free_block": 0,
            "frame_scratch_bytes": 16 + (point_count * 2),
            "packet_scratch_bytes": 20 + (point_count * 4) + 28 + 4 + 32,
            "filter_state_bytes": point_count,
            "calibration_bytes": sum(len(points) for points in self.calibration_table) * 8,
            "ring_bytes": self.buffer_frames * (16 + (point_count * 2)),
        }


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
