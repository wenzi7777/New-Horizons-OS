# offline_recorder.py
import gc
import time

import config
import fs_core


def _ticks_ms():
    return time.ticks_ms() if hasattr(time, "ticks_ms") else int(time.time() * 1000)


def _ticks_diff(now, then):
    return time.ticks_diff(now, then) if hasattr(time, "ticks_diff") else now - then


class OfflineRecorder:
    EXTENSION = ".nhr"
    RECORD_HEADER_LEN = 4

    def __init__(self, root=None, logger=None):
        self.root = root or getattr(config, "OFFLINE_RECORD_DIR", "data/offline")
        self.logger = logger
        self.active = False
        self.eligible = False
        self.rolling = False
        self.bytes_used = 0
        self.bytes_limit = 0
        self.segment_count = 0
        self.dropped_frames = 0
        self.stop_reason = ""
        self.error = ""
        self.estimated_seconds_until_rollover = 0
        self.current_file = None
        self.current_path = ""
        self.current_bytes = 0
        self.next_index = 0
        self.segments = []
        self.record_header = bytearray(self.RECORD_HEADER_LEN)
        self.last_estimate_ms = 0
        self.last_flush_ms = 0
        self.estimate_window_ms = 0
        self.estimate_window_bytes = 0
        self.observed_bytes_per_second = 0
        self.write_backoff_until_ms = 0

    def set_eligible(self, eligible):
        self.eligible = bool(eligible)

    def begin(self, now_ms=None):
        now_ms = _ticks_ms() if now_ms is None else now_ms
        if self.active:
            return True, "already_recording"
        gc.collect()
        self.stop_reason = ""
        self.error = ""
        self.rolling = False
        self.dropped_frames = 0
        self.write_backoff_until_ms = 0
        fs_core.ensure_dir(self.root)
        self._refresh_segments()
        usage = fs_core.statvfs_usage("/")
        total = int(usage.get("total_bytes", 0) or 0)
        free = int(usage.get("free_bytes", 0) or 0)
        reserve = max(
            int(getattr(config, "OFFLINE_RECORD_RESERVE_BYTES", 1048576)),
            (total * int(getattr(config, "OFFLINE_RECORD_RESERVE_PERCENT", 10))) // 100 if total else 0,
        )
        writable = max(0, free - reserve)
        self.bytes_limit = self.bytes_used + writable
        minimum = int(getattr(config, "OFFLINE_RECORD_MIN_USABLE_BYTES", 524288))
        if self.bytes_limit < minimum:
            self.error = "offline_storage_low"
            return False, self.error
        self.active = True
        self.last_estimate_ms = now_ms
        self.last_flush_ms = now_ms
        self.estimate_window_ms = now_ms
        self.estimate_window_bytes = 0
        self.observed_bytes_per_second = 0
        if self.logger:
            self.logger.info("offline_recording_started bytes_limit={}".format(self.bytes_limit))
        return True, "offline_recording_started"

    def stop(self, reason="stopped"):
        self._close_current()
        if self.active and self.logger:
            self.logger.info("offline_recording_stopped reason={}".format(reason))
        self.active = False
        self.stop_reason = reason
        return True

    def write_packet(self, packet, now_ms=None):
        if not self.active:
            return False
        now_ms = _ticks_ms() if now_ms is None else now_ms
        if self.write_backoff_until_ms and _ticks_diff(now_ms, self.write_backoff_until_ms) < 0:
            self.dropped_frames += 1
            return False
        length = len(packet)
        if length <= 0 or length > 0xffff:
            self.dropped_frames += 1
            self.error = "offline_packet_invalid"
            return False
        record_len = self.RECORD_HEADER_LEN + length
        if self.current_file is None or self.current_bytes + record_len > int(getattr(config, "OFFLINE_RECORD_SEGMENT_BYTES", 262144)):
            if not self._rotate_segment():
                self.dropped_frames += 1
                return False
        if not self._ensure_capacity(record_len):
            self.dropped_frames += 1
            self.error = "offline_storage_full"
            return False

        self.record_header[0] = length & 0xff
        self.record_header[1] = (length >> 8) & 0xff
        inv = length ^ 0xffff
        self.record_header[2] = inv & 0xff
        self.record_header[3] = (inv >> 8) & 0xff

        start_ms = _ticks_ms()
        try:
            self.current_file.write(self.record_header)
            self.current_file.write(packet)
        except Exception as exc:
            self.error = "offline_write_failed"
            self.stop(self.error)
            if self.logger:
                self.logger.warn("offline_recording_write_failed {}".format(exc))
            return False

        self.current_bytes += record_len
        self.bytes_used += record_len
        self.estimate_window_bytes += record_len
        elapsed = _ticks_diff(_ticks_ms(), start_ms)
        budget = int(getattr(config, "OFFLINE_RECORD_WRITE_BUDGET_MS", 8))
        if elapsed > budget:
            self.write_backoff_until_ms = time.ticks_add(now_ms, int(getattr(config, "OFFLINE_RECORD_WRITE_BACKOFF_MS", 50))) if hasattr(time, "ticks_add") else now_ms + int(getattr(config, "OFFLINE_RECORD_WRITE_BACKOFF_MS", 50))
        self._maybe_flush(now_ms)
        self._maybe_update_estimate(now_ms)
        return True

    def status(self):
        return {
            "eligible": bool(self.eligible),
            "active": bool(self.active),
            "rolling": bool(self.rolling),
            "bytes_used": int(self.bytes_used),
            "bytes_limit": int(self.bytes_limit),
            "estimated_seconds_until_rollover": int(self.estimated_seconds_until_rollover),
            "segment_count": int(self.segment_count),
            "dropped_frames": int(self.dropped_frames),
            "stop_reason": self.stop_reason,
            "error": self.error,
        }

    def led_bucket(self):
        if self.error:
            return "error"
        if self.rolling:
            return "rolling"
        seconds = int(self.estimated_seconds_until_rollover or 0)
        if seconds <= 0 or seconds > 60:
            return "ok"
        if seconds > 30:
            return "low"
        if seconds > 10:
            return "critical"
        return "urgent"

    def _refresh_segments(self):
        segments = []
        max_index = -1
        total = 0
        for name in fs_core.list_names(self.root):
            if not name.endswith(self.EXTENSION):
                continue
            stem = name[:-len(self.EXTENSION)]
            try:
                index = int(stem.rsplit("_", 1)[-1])
            except (ValueError, TypeError):
                continue
            path = self.root.rstrip("/") + "/" + name
            size = fs_core.file_size(path)
            if size is None:
                continue
            segments.append({"index": index, "path": path, "size": int(size)})
            total += int(size)
            if index > max_index:
                max_index = index
        segments.sort(key=lambda item: int(item["index"]))
        self.segments = segments
        self.segment_count = len(segments)
        self.bytes_used = total
        self.next_index = max_index + 1

    def _rotate_segment(self):
        self._close_current()
        path = self.root.rstrip("/") + "/offline_{:06d}{}".format(self.next_index, self.EXTENSION)
        self.next_index += 1
        try:
            fs_core.ensure_dir(fs_core.dirname(path))
            self.current_file = open(path, "wb")
        except Exception as exc:
            self.error = "offline_segment_open_failed"
            if self.logger:
                self.logger.warn("offline_segment_open_failed {}".format(exc))
            return False
        self.current_path = path
        self.current_bytes = 0
        self.segments.append({"index": self.next_index - 1, "path": path, "size": 0})
        self.segment_count = len(self.segments)
        return True

    def _close_current(self):
        if self.current_file is not None:
            try:
                self.current_file.flush()
            except Exception:
                pass
            try:
                self.current_file.close()
            except Exception:
                pass
        if self.current_path:
            for item in self.segments:
                if item.get("path") == self.current_path:
                    item["size"] = self.current_bytes
                    break
        self.current_file = None
        self.current_path = ""
        self.current_bytes = 0

    def _ensure_capacity(self, record_len):
        if self.bytes_limit <= 0:
            return False
        while self.bytes_used + record_len > self.bytes_limit:
            if not self._delete_oldest_sealed_segment():
                return False
            self.rolling = True
        return True

    def _delete_oldest_sealed_segment(self):
        for item in list(self.segments):
            path = item.get("path")
            if not path or path == self.current_path:
                continue
            if fs_core.remove(path):
                self.bytes_used = max(0, self.bytes_used - int(item.get("size", 0) or 0))
                self.segments.remove(item)
                self.segment_count = len(self.segments)
                return True
        return False

    def _maybe_flush(self, now_ms):
        interval = int(getattr(config, "OFFLINE_RECORD_FLUSH_INTERVAL_MS", 1000))
        if interval <= 0 or self.current_file is None:
            return
        if _ticks_diff(now_ms, self.last_flush_ms) < interval:
            return
        try:
            self.current_file.flush()
        except Exception:
            pass
        self.last_flush_ms = now_ms

    def _maybe_update_estimate(self, now_ms):
        interval = int(getattr(config, "OFFLINE_RECORD_ESTIMATE_INTERVAL_MS", 5000))
        if _ticks_diff(now_ms, self.estimate_window_ms) < interval:
            return
        elapsed_ms = max(1, _ticks_diff(now_ms, self.estimate_window_ms))
        self.observed_bytes_per_second = int((self.estimate_window_bytes * 1000) // elapsed_ms)
        self.estimate_window_ms = now_ms
        self.estimate_window_bytes = 0
        remaining = max(0, int(self.bytes_limit) - int(self.bytes_used))
        if self.observed_bytes_per_second > 0:
            self.estimated_seconds_until_rollover = remaining // self.observed_bytes_per_second
        else:
            self.estimated_seconds_until_rollover = 0
