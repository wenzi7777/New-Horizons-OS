try:
    import uos as os
except ImportError:  # pragma: no cover - CPython fallback
    import os
import time

import storage


DEBUG = 10
INFO = 20
WARN = 30
ERROR = 40

DEFAULT_LOG_BYTES = 16384
EXTENDED_LOG_BYTES = 65536
MAX_LINE_CHARS = 256
VALID_CAPACITIES = ("default", "extended")


class DeviceLogger:
    def __init__(
        self,
        path="data/logs/device.log",
        max_bytes=None,
        level=INFO,
        enabled=True,
        capacity="default",
        serial="status",
    ):
        self.path = path
        self.level = int(level)
        self.max_line_chars = MAX_LINE_CHARS
        self.serial = serial or "status"
        self.enabled = bool(enabled)
        self.capacity = self._normalize_capacity(capacity)
        self.max_bytes = int(max_bytes) if max_bytes is not None else self._bytes_for_capacity(self.capacity)
        self.segment_bytes = max(1, int(self.max_bytes // 2))

    def configure(self, enabled=None, capacity=None):
        if enabled is not None:
            self.enabled = bool(enabled)
        if capacity is not None:
            self.capacity = self._normalize_capacity(capacity)
            self.max_bytes = self._bytes_for_capacity(self.capacity)
            self.segment_bytes = max(1, int(self.max_bytes // 2))

    def settings(self):
        return {
            "enabled": bool(self.enabled),
            "capacity": self.capacity,
            "serial": self.serial,
            "max_bytes": int(self.max_bytes),
        }

    def debug(self, message):
        self.log(DEBUG, "DEBUG", message)

    def info(self, message):
        self.log(INFO, "INFO", message)

    def warn(self, message):
        self.log(WARN, "WARN", message)

    def error(self, message):
        self.log(ERROR, "ERROR", message)

    def log(self, level_value, level_name, message):
        if int(level_value) < self.level:
            return
        line = self._format_line(level_name, message)
        print(line, end="")
        if self.enabled:
            self._append_line(line)

    def read_tail(self, max_lines=50):
        limit = max(0, int(max_lines))
        if limit == 0:
            return []
        lines = []
        for path in (self.path + ".1", self.path):
            self._read_tail_file(path, lines, limit)
        return lines

    def clear(self):
        removed = False
        for path in (self.path, self.path + ".1"):
            try:
                os.remove(path)
                removed = True
            except OSError:
                pass
        return removed

    def _read_tail_file(self, path, lines, limit):
        try:
            with open(path, "r") as f:
                for line in f:
                    line = line.rstrip("\n")
                    if not line:
                        continue
                    lines.append(line)
                    if len(lines) > limit:
                        lines.pop(0)
        except OSError:
            return

    def _append_line(self, line):
        encoded_len = len(line.encode())
        storage.ensure_dir(storage.dirname(self.path))
        current_size = self._file_size()
        if current_size and current_size + encoded_len > self.segment_bytes:
            self._rotate()
        with open(self.path, "a") as f:
            f.write(line)

    def _file_size(self):
        try:
            return int(os.stat(self.path)[6])
        except OSError:
            return 0

    def _rotate(self):
        backup = self.path + ".1"
        try:
            os.remove(backup)
        except OSError:
            pass
        try:
            os.rename(self.path, backup)
        except OSError:
            pass

    def _format_line(self, level_name, message):
        prefix = "{} [{}] ".format(self._timestamp(), level_name)
        message = str(message)
        max_message_chars = max(0, self.max_line_chars - len(prefix))
        if len(prefix) + len(message) > self.max_line_chars:
            if max_message_chars > 3:
                message = message[:max_message_chars - 3] + "..."
            else:
                message = message[:max_message_chars]
        return prefix + message + "\n"

    def _normalize_capacity(self, capacity):
        capacity = str(capacity or "default")
        return capacity if capacity in VALID_CAPACITIES else "default"

    def _bytes_for_capacity(self, capacity):
        return EXTENDED_LOG_BYTES if capacity == "extended" else DEFAULT_LOG_BYTES

    def _timestamp(self):
        try:
            return str(time.time())
        except Exception:
            return "0"
